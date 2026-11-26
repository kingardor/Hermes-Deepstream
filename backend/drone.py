"""Ryze Tello control for the Hermes web UI.

Replaces the old tello-control.py:
- No global keyboard hook (pynput captured keys system-wide). Commands now come
  from the web UI, and only while the operator has armed the controls.
- Fixes swapped yaw: "rotate left" is counter-clockwise.
- Exposes telemetry (battery, height, flight time, temperature, ...) that the
  old get_status() collected but never showed.
- Safety: one command at a time, a keepalive so the Tello doesn't auto-land
  after 15 s of silence, and an auto-land if the operator's UI disconnects.
- Relays the Tello camera as RTSP (rtsp://127.0.0.1:6969/hermes) so DeepStream
  can ingest it, as before, but without the Redis frame hop.
"""
import logging
import queue
import random
import threading
import time
from typing import Any, Callable, Dict, Optional

log = logging.getLogger("hermes.drone")

STEP_CM = 30          # same step sizes as the original keyboard controls
STEP_DEG = 30
HEARTBEAT_TIMEOUT_S = 10.0   # land if the controlling UI is gone this long while flying
KEEPALIVE_S = 5.0            # Tello auto-lands after ~15 s without any command
RC_TIMEOUT_S = 0.5           # gamepad sticks: zero the RC if updates stop

MOTION = {
    "forward": ("move_forward", STEP_CM), "back": ("move_back", STEP_CM),
    "left": ("move_left", STEP_CM), "right": ("move_right", STEP_CM),
    "up": ("move_up", STEP_CM), "down": ("move_down", STEP_CM),
    # Fixed: the old script sent rotate_clockwise for the LEFT arrow.
    "yaw_left": ("rotate_counter_clockwise", STEP_DEG),
    "yaw_right": ("rotate_clockwise", STEP_DEG),
}


class CommandError(Exception):
    pass


class BaseDrone:
    """State machine shared by the real and the demo drone."""

    available = True

    def __init__(self) -> None:
        self.connected = False
        self.flying = False
        self.armed = False
        self.busy: Optional[str] = None
        self.last_error = ""
        self.telemetry: Dict[str, Any] = {}
        self._heartbeat = time.time()
        self._rc_at = 0.0
        self._emergency_at = 0.0
        self._queue: "queue.Queue[tuple]" = queue.Queue()
        self._lock = threading.Lock()
        self._on_change: Optional[Callable[[], None]] = None

    # -- public API ---------------------------------------------------------
    def state(self) -> Dict[str, Any]:
        return {
            "available": self.available,
            "connected": self.connected,
            "flying": self.flying,
            "armed": self.armed,
            "busy": self.busy,
            "error": self.last_error,
            "telemetry": self.telemetry,
            "step_cm": STEP_CM,
            "step_deg": STEP_DEG,
        }

    def heartbeat(self) -> None:
        self._heartbeat = time.time()

    def set_armed(self, armed: bool) -> None:
        self.armed = bool(armed) and self.connected
        self._changed()

    def command(self, name: str) -> None:
        """Queue a command. Raises CommandError if it isn't allowed right now."""
        if not self.connected:
            raise CommandError("The drone isn't connected.")
        if name == "emergency":
            # Always allowed, and never queued behind a slow move: cut the motors now.
            threading.Thread(target=self._emergency_now, name="drone-emergency", daemon=True).start()
            return
        if not self.armed:
            raise CommandError("Arm the controls first.")
        if self.busy:
            raise CommandError("Still running: %s" % self.busy)
        if name == "takeoff":
            if self.flying:
                raise CommandError("Already flying.")
        elif name == "land":
            if not self.flying:
                raise CommandError("Not flying.")
        elif name in MOTION:
            if not self.flying:
                raise CommandError("Take off first.")
        else:
            raise CommandError("Unknown command: %s" % name)
        self.busy = name
        self.last_error = ""   # a new operator command clears the previous notice
        self._changed()
        self._queue.put((name,))

    def rc(self, lr: int, fb: int, ud: int, yaw: int) -> None:
        """Continuous stick control (gamepad), -100..100 per axis."""
        if not (self.connected and self.armed and self.flying) or self.busy:
            return
        clamp = lambda v: max(-100, min(100, int(v)))
        self._rc_at = time.time()
        self._send_rc(clamp(lr), clamp(fb), clamp(ud), clamp(yaw))

    # -- internals ------------------------------------------------------------
    def _changed(self) -> None:
        if self._on_change:
            self._on_change()

    def _emergency_now(self) -> None:
        self._emergency_at = time.time()
        try:
            self._do_emergency()
            self.last_error = "Emergency stop: motors cut."
        except Exception as exc:  # noqa: BLE001
            self.last_error = "Emergency stop failed: %s" % exc
        self.flying = False
        self.armed = False
        self._changed()

    def _worker(self) -> None:
        while True:
            item = self._queue.get()
            name = item[0]
            started = time.time()
            try:
                if name == "takeoff":
                    self._do("takeoff")
                    # an emergency stop during take-off wins
                    self.flying = self._emergency_at < started
                elif name == "land":
                    self._do("land")
                    self.flying = False
                elif name in MOTION:
                    method, amount = MOTION[name]
                    self._do(method, amount)
            except Exception as exc:  # noqa: BLE001 - report every drone failure to the UI
                log.warning("command %s failed: %s", name, exc)
                self.last_error = "%s failed: %s" % (name, exc)
            finally:
                self.busy = None
                self._changed()

    def _safety_loop(self) -> None:
        last_keepalive = time.time()
        rc_zeroed = True
        while True:
            time.sleep(0.25)
            now = time.time()
            if not self.flying:
                continue
            # Stop gamepad drift when stick updates stop arriving
            if self._rc_at and now - self._rc_at > RC_TIMEOUT_S and not rc_zeroed:
                self._send_rc(0, 0, 0, 0)
                rc_zeroed = True
            if self._rc_at and now - self._rc_at <= RC_TIMEOUT_S:
                rc_zeroed = False
            # Operator gone: land instead of hovering until the battery dies
            if now - self._heartbeat > HEARTBEAT_TIMEOUT_S and not self.busy:
                log.warning("no operator heartbeat for %.0fs, landing", now - self._heartbeat)
                self.last_error = "Operator disconnected: landed automatically and disarmed."
                self.armed = False
                self.busy = "land"
                self._queue.put(("land",))
                self._heartbeat = now
                self._changed()
                continue
            # Keep the Tello from auto-landing while it hovers
            if now - last_keepalive > KEEPALIVE_S and not self.busy:
                try:
                    self._keepalive()
                except Exception as exc:  # noqa: BLE001
                    log.debug("keepalive failed: %s", exc)
                last_keepalive = now

    def _start_threads(self) -> None:
        threading.Thread(target=self._worker, name="drone-worker", daemon=True).start()
        threading.Thread(target=self._safety_loop, name="drone-safety", daemon=True).start()

    # implemented by subclasses
    def _do(self, method: str, *args: Any) -> None:
        raise NotImplementedError

    def _do_emergency(self) -> None:
        raise NotImplementedError

    def _send_rc(self, lr: int, fb: int, ud: int, yaw: int) -> None:
        raise NotImplementedError

    def _keepalive(self) -> None:
        raise NotImplementedError


class TelloDrone(BaseDrone):
    """A real Ryze Tello via djitellopy 2.x."""

    def __init__(self, relay: bool = True, rtsp_port: int = 6969, rtsp_path: str = "/hermes") -> None:
        super().__init__()
        from djitellopy import Tello  # imported lazily so the backend runs without a drone

        self.tello = Tello()
        self.relay = relay
        self.rtsp_port = rtsp_port
        self.rtsp_path = rtsp_path

    def start(self) -> None:
        self.tello.connect()
        self.connected = True
        self._start_threads()
        threading.Thread(target=self._telemetry_loop, name="drone-telemetry", daemon=True).start()
        if self.relay:
            self.tello.streamon()
            relay = RtspRelay(self.tello, self.rtsp_port, self.rtsp_path)
            threading.Thread(target=relay.run, name="drone-rtsp", daemon=True).start()
        log.info("Tello connected (battery %s%%)", self.tello.get_battery())

    def _telemetry_loop(self) -> None:
        t = self.tello
        while True:
            try:
                # djitellopy 2.x reads these from the Tello's state packets (no extra round-trips)
                self.telemetry = {
                    "battery": t.get_battery(),
                    "flight_time": t.get_flight_time(),
                    "height_cm": t.get_height(),
                    "barometer_cm": t.get_barometer(),
                    "temperature": t.get_temperature(),
                    "yaw": t.get_yaw(),
                    "speed": [t.get_speed_x(), t.get_speed_y(), t.get_speed_z()],
                }
                self._changed()
            except Exception as exc:  # noqa: BLE001
                log.debug("telemetry read failed: %s", exc)
            time.sleep(0.5)

    def _do(self, method: str, *args: Any) -> None:
        getattr(self.tello, method)(*args)

    def _do_emergency(self) -> None:
        self.tello.emergency()

    def _send_rc(self, lr: int, fb: int, ud: int, yaw: int) -> None:
        self.tello.send_rc_control(lr, fb, ud, yaw)

    def _keepalive(self) -> None:
        if hasattr(self.tello, "send_keepalive"):
            self.tello.send_keepalive()
        else:
            self.tello.send_rc_control(0, 0, 0, 0)


class RtspRelay:
    """Serve the Tello camera as RTSP so the DeepStream app can use it as a source."""

    WIDTH, HEIGHT, FPS = 960, 720, 30

    def __init__(self, tello: Any, port: int, path: str) -> None:
        self.tello = tello
        self.port = port
        self.path = path

    def run(self) -> None:
        import gi
        gi.require_version("Gst", "1.0")
        gi.require_version("GstRtspServer", "1.0")
        from gi.repository import GLib, Gst, GstRtspServer

        Gst.init(None)
        frame_read = self.tello.get_frame_read()
        duration = Gst.SECOND // self.FPS
        relay = self

        class Factory(GstRtspServer.RTSPMediaFactory):
            def __init__(self) -> None:
                super().__init__()
                self.n = 0
                self.set_shared(True)
                # djitellopy 2.4 decodes frames as BGR
                self.launch = (
                    "appsrc name=source is-live=true block=true format=GST_FORMAT_TIME "
                    "caps=video/x-raw,format=BGR,width={w},height={h},framerate={f}/1 "
                    "! videoconvert ! video/x-raw,format=I420 "
                    "! x264enc speed-preset=ultrafast tune=zerolatency "
                    "! rtph264pay config-interval=1 name=pay0 pt=96"
                ).format(w=relay.WIDTH, h=relay.HEIGHT, f=relay.FPS)

            def do_create_element(self, url: Any) -> Any:
                return Gst.parse_launch(self.launch)

            def do_configure(self, media: Any) -> None:
                self.n = 0
                media.get_element().get_child_by_name("source").connect("need-data", self.on_need_data)

            def on_need_data(self, src: Any, length: int) -> None:
                frame = frame_read.frame
                if frame is None:
                    return
                data = frame.tobytes()  # tostring() is deprecated
                buf = Gst.Buffer.new_allocate(None, len(data), None)
                buf.fill(0, data)
                buf.duration = duration
                buf.pts = buf.dts = self.n * duration
                self.n += 1
                src.emit("push-buffer", buf)

        server = GstRtspServer.RTSPServer()
        server.set_service(str(self.port))
        server.get_mount_points().add_factory(self.path, Factory())
        server.attach(None)
        log.info("Tello camera relayed at rtsp://127.0.0.1:%d%s", self.port, self.path)
        GLib.MainLoop().run()


class DemoDrone(BaseDrone):
    """Simulated Tello for UI development (no hardware)."""

    def __init__(self) -> None:
        super().__init__()
        self._t0 = time.time()
        self._height = 0
        self._yaw = 212
        self._takeoff_at: Optional[float] = None

    def start(self) -> None:
        self.connected = True
        self._start_threads()
        threading.Thread(target=self._telemetry_loop, name="demo-telemetry", daemon=True).start()

    def _telemetry_loop(self) -> None:
        battery = 78.0
        while True:
            if self.flying:
                battery = max(5.0, battery - 0.05)
            flight = int(time.time() - self._takeoff_at) if (self.flying and self._takeoff_at) else 0
            self.telemetry = {
                "battery": int(battery),
                "flight_time": flight,
                "height_cm": self._height + (random.randint(-2, 2) if self.flying else 0),
                "barometer_cm": 9120 + self._height,
                "temperature": 46 + (2 if self.flying else 0),
                "yaw": self._yaw,
                "speed": [random.randint(-2, 2) if self.flying else 0, 0, 0],
            }
            self._changed()
            time.sleep(0.5)

    def _do(self, method: str, *args: Any) -> None:
        time.sleep(1.2 if method in ("takeoff", "land") else 0.7)
        if method == "takeoff":
            self._height, self._takeoff_at = 120, time.time()
        elif method == "land":
            self._height = 0
        elif method == "move_up":
            self._height += args[0]
        elif method == "move_down":
            self._height = max(20, self._height - args[0])
        elif method == "rotate_clockwise":
            self._yaw = (self._yaw + args[0]) % 360
        elif method == "rotate_counter_clockwise":
            self._yaw = (self._yaw - args[0]) % 360

    def _do_emergency(self) -> None:
        self._height = 0

    def _send_rc(self, lr: int, fb: int, ud: int, yaw: int) -> None:
        self._yaw = (self._yaw + yaw // 20) % 360

    def _keepalive(self) -> None:
        pass


class NoDrone(BaseDrone):
    available = False

    def start(self) -> None:
        pass
