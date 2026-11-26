"""Hermes web UI backend.

Bridges the DeepStream app and the browser:
- subscribes to detections the app publishes on Redis (hermes:detections)
- turns them into fire events + a timeline (events.py)
- reports pipeline and device health (health.py)
- owns the Tello drone and exposes safe controls (drone.py)
- serves the web UI (../web) and pushes everything over one WebSocket

Run:  python3 backend/hermes_backend.py [--drone] [--host 0.0.0.0] [--token SECRET]
Demo: python3 backend/hermes_backend.py --demo      (no Jetson, Redis or drone needed)
"""
import argparse
import asyncio
import base64
import json
import logging
import re
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Set
from urllib.parse import urlparse

sys.path.insert(0, str(Path(__file__).resolve().parent))

import uvicorn  # noqa: E402
from fastapi import FastAPI, HTTPException, Request, WebSocket, WebSocketDisconnect  # noqa: E402
from fastapi.responses import FileResponse  # noqa: E402
from fastapi.staticfiles import StaticFiles  # noqa: E402

import demo  # noqa: E402
import health  # noqa: E402
from drone import BaseDrone, CommandError, DemoDrone, NoDrone  # noqa: E402
from events import EventTracker  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
WEB_DIR = ROOT / "web"
DATA_DIR = ROOT / "data"
THUMBS_DIR = DATA_DIR / "thumbs"

DETECTIONS_CHANNEL = "hermes:detections"
PIPELINE_KEY = "hermes:pipeline"
DEFAULT_DRONE_URI = "rtsp://127.0.0.1:6969/hermes"
STALE_S = 3.0

log = logging.getLogger("hermes")


def display_uri(uri: str) -> str:
    """Never send stream credentials to the browser."""
    p = urlparse(uri)
    if p.username or p.password:
        host = p.hostname or ""
        if p.port:
            host += ":%d" % p.port
        return p._replace(netloc=host).geturl()
    return uri


def source_name(uri: str) -> str:
    p = urlparse(uri)
    if p.scheme == "file":
        return Path(p.path).name or uri
    return p.hostname or uri


class Hub:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.clients: Set[WebSocket] = set()
        # Demo mode keeps its events in memory, out of the real event log
        self.tracker = EventTracker(None if args.demo else str(DATA_DIR / "events.db"))
        self.pipeline: Dict[str, Any] = {}
        self.latest: Dict[int, Dict[str, Any]] = {}
        self.last_msg = 0.0
        self.dirty_dets: Set[int] = set()
        self.dirty_events: Set[str] = set()
        self.drone: BaseDrone = NoDrone()
        self.drone_dirty: Optional[asyncio.Event] = None   # created on the server's loop at startup
        self.loop: Optional[asyncio.AbstractEventLoop] = None

    # -- pipeline input -------------------------------------------------------
    def set_pipeline(self, info: Dict[str, Any]) -> None:
        started = str(info.get("started", ""))
        if started and started != str(self.pipeline.get("started", "")):
            self.tracker.run = started[-6:] + ":"
        self.pipeline = info

    def on_frame(self, msg: Dict[str, Any]) -> None:
        src = int(msg.get("src", 0))
        self.latest[src] = {"t": msg.get("t"), "fps": msg.get("fps", 0), "objs": msg.get("objs") or []}
        self.last_msg = time.time()
        self.dirty_dets.add(src)
        for event_id in self.tracker.ingest(msg):
            self.dirty_events.add(event_id)

    # -- config sent to clients -------------------------------------------------
    def config(self) -> Dict[str, Any]:
        sources = self.pipeline.get("sources") or []
        cols = int(self.pipeline.get("columns") or 1)
        drone_uri = self.args.drone_uri
        return {
            "demo": self.args.demo,
            "grid": {"rows": int(self.pipeline.get("rows") or 1), "cols": cols,
                     "width": int(self.pipeline.get("tile_width") or 1920),
                     "height": int(self.pipeline.get("tile_height") or 1080),
                     "src_width": int(self.pipeline.get("width") or 1920),
                     "src_height": int(self.pipeline.get("height") or 1080)},
            "sources": [{
                "index": i, "name": "DRONE" if uri == drone_uri else source_name(uri),
                "uri": display_uri(uri), "row": i // cols, "col": i % cols,
                "is_drone": uri == drone_uri,
            } for i, uri in enumerate(sources)],
            "video": {"mode": "demo" if self.args.demo else "webrtc",
                      "whep_port": self.args.whep_port, "whep_path": self.args.whep_path,
                      "poster": "/demo/wildfires.png" if self.args.demo else None},
            "drone": self.drone.state(),
        }

    def health(self) -> Dict[str, Any]:
        age = time.time() - self.last_msg if self.last_msg else None
        state = "offline" if age is None else ("running" if age <= STALE_S else "stalled")
        return {
            "pipeline": {"state": state, "age": age,
                         "fps": {str(k): v.get("fps", 0) for k, v in self.latest.items()}},
            "device": health.device(),
        }

    # -- broadcasting ---------------------------------------------------------
    async def send(self, ws: WebSocket, msg: Dict[str, Any]) -> None:
        try:
            await ws.send_text(json.dumps(msg))
        except Exception:  # noqa: BLE001 - client went away
            self.clients.discard(ws)

    async def broadcast(self, msg: Dict[str, Any]) -> None:
        for ws in list(self.clients):
            await self.send(ws, msg)

    async def pump(self) -> None:
        """Push changes to clients: detections 5x/s, health every second, timeline every 2 s."""
        tick = 0
        while True:
            await asyncio.sleep(0.2)
            tick += 1
            if self.dirty_dets:
                dets = {str(s): self.latest[s]["objs"] for s in self.dirty_dets if s in self.latest}
                self.dirty_dets.clear()
                await self.broadcast({"type": "dets", "dets": dets})
            if self.dirty_events:
                ids, self.dirty_events = self.dirty_events, set()
                evs = [e for e in (self.tracker.get(i) for i in ids) if e]
                if evs:
                    await self.broadcast({"type": "events", "events": evs})
            if tick % 5 == 0:
                await self.broadcast({"type": "health", "health": self.health()})
                # statuses (new -> active -> ended) change with time too
                await self.broadcast({"type": "events", "events": self.tracker.snapshot(), "full": True})
            if tick % 10 == 0:
                await self.broadcast({"type": "timeline", "timeline": self.tracker.timeline(), "now": time.time()})
            if tick % 50 == 0:
                await asyncio.get_running_loop().run_in_executor(None, self.tracker.flush)

    async def drone_pump(self) -> None:
        assert self.drone_dirty is not None
        while True:
            await self.drone_dirty.wait()
            self.drone_dirty.clear()
            await self.broadcast({"type": "drone", "drone": self.drone.state()})
            await asyncio.sleep(0.2)  # at most 5 updates/s

    def drone_changed(self) -> None:
        if self.loop and self.drone_dirty is not None:
            self.loop.call_soon_threadsafe(self.drone_dirty.set)


# -- inputs -------------------------------------------------------------------------
async def redis_listener(hub: Hub, host: str, port: int) -> None:
    import redis.asyncio as aioredis

    while True:
        try:
            client = aioredis.Redis(host=host, port=port, decode_responses=True)
            info = await client.hgetall(PIPELINE_KEY)
            if info:
                hub.set_pipeline(_parse_pipeline(info))
            pubsub = client.pubsub()
            await pubsub.subscribe(DETECTIONS_CHANNEL)
            log.info("Listening for detections on redis://%s:%d %s", host, port, DETECTIONS_CHANNEL)
            last_info = time.time()
            async for message in pubsub.listen():
                if message.get("type") != "message":
                    continue
                try:
                    hub.on_frame(json.loads(message["data"]))
                except (ValueError, KeyError) as exc:
                    log.debug("bad detection message: %s", exc)
                if time.time() - last_info > 5:   # pick up pipeline restarts / source changes
                    last_info = time.time()
                    info = await client.hgetall(PIPELINE_KEY)
                    if info:
                        before = hub.pipeline.get("started")
                        hub.set_pipeline(_parse_pipeline(info))
                        if hub.pipeline.get("started") != before:
                            await hub.broadcast({"type": "hello", "config": hub.config()})
        except Exception as exc:  # noqa: BLE001 - keep retrying while the pipeline is down
            log.warning("Redis unavailable (%s); retrying in 3 s", exc)
            await asyncio.sleep(3)


def _parse_pipeline(info: Dict[str, str]) -> Dict[str, Any]:
    out: Dict[str, Any] = dict(info)
    try:
        out["sources"] = json.loads(info.get("sources", "[]"))
    except ValueError:
        out["sources"] = []
    return out


async def demo_feed(hub: Hub) -> None:
    start = time.time()
    hub.set_pipeline(dict(demo.PIPELINE, started=int(start * 1000)))
    demo.seed(hub.on_frame, start)
    for event_id in demo.ACKNOWLEDGED:
        hub.tracker.acknowledge(hub.tracker.run + event_id)
    while True:
        now = time.time()
        for src in range(4):
            hub.on_frame(demo.frame(src, now, start))
        await asyncio.sleep(0.1)


# -- app ------------------------------------------------------------------------------
def create_app(args: argparse.Namespace) -> FastAPI:
    app = FastAPI(title="Hermes", docs_url=None, redoc_url=None)
    hub = Hub(args)
    THUMBS_DIR.mkdir(parents=True, exist_ok=True)

    def authorized(token: Optional[str]) -> bool:
        return not args.token or token == args.token

    @app.on_event("startup")
    async def startup() -> None:
        hub.loop = asyncio.get_running_loop()
        hub.drone_dirty = asyncio.Event()
        if args.demo:
            hub.drone = DemoDrone()
        elif args.drone:
            from drone import TelloDrone
            hub.drone = TelloDrone(relay=not args.no_relay)
        hub.drone._on_change = hub.drone_changed
        try:
            await asyncio.get_running_loop().run_in_executor(None, hub.drone.start)
        except Exception as exc:  # noqa: BLE001
            log.error("Drone failed to start: %s", exc)
            hub.drone.last_error = "Couldn't connect to the drone: %s" % exc
        asyncio.create_task(hub.pump())
        asyncio.create_task(hub.drone_pump())
        if args.demo:
            asyncio.create_task(demo_feed(hub))
        else:
            asyncio.create_task(redis_listener(hub, args.redis_host, args.redis_port))

    @app.websocket("/ws")
    async def ws_endpoint(ws: WebSocket) -> None:
        if not authorized(ws.query_params.get("token")):
            await ws.close(code=4401)
            return
        await ws.accept()
        hub.clients.add(ws)
        hub.drone.heartbeat()
        await hub.send(ws, {"type": "hello", "config": hub.config()})
        await hub.send(ws, {"type": "events", "events": hub.tracker.snapshot(), "full": True})
        await hub.send(ws, {"type": "timeline", "timeline": hub.tracker.timeline(), "now": time.time()})
        await hub.send(ws, {"type": "health", "health": hub.health()})
        try:
            while True:
                msg = json.loads(await ws.receive_text())
                kind = msg.get("type")
                if kind == "heartbeat":
                    hub.drone.heartbeat()
                elif kind == "ack":
                    ev = hub.tracker.acknowledge(str(msg.get("id")))
                    if ev:
                        await hub.broadcast({"type": "events", "events": [ev]})
                elif kind == "arm":
                    hub.drone.set_armed(bool(msg.get("on")))
                elif kind == "drone":
                    try:
                        hub.drone.command(str(msg.get("cmd")))
                        await hub.send(ws, {"type": "drone_result", "ok": True, "cmd": msg.get("cmd")})
                    except CommandError as exc:
                        await hub.send(ws, {"type": "drone_result", "ok": False, "cmd": msg.get("cmd"), "error": str(exc)})
                elif kind == "rc":
                    hub.drone.heartbeat()
                    hub.drone.rc(msg.get("lr", 0), msg.get("fb", 0), msg.get("ud", 0), msg.get("yaw", 0))
        except (WebSocketDisconnect, RuntimeError, ValueError):
            pass
        finally:
            hub.clients.discard(ws)

    @app.get("/api/config")
    async def api_config(token: Optional[str] = None) -> Dict[str, Any]:
        if not authorized(token):
            raise HTTPException(401)
        return hub.config()

    @app.get("/api/events")
    async def api_events(token: Optional[str] = None) -> List[Dict[str, Any]]:
        if not authorized(token):
            raise HTTPException(401)
        return hub.tracker.snapshot()

    @app.post("/api/events/{event_id}/thumb")
    async def api_thumb(event_id: str, request: Request, token: Optional[str] = None) -> Dict[str, Any]:
        """The browser grabs a crop of the fire from the live video and stores it here."""
        if not authorized(token):
            raise HTTPException(401)
        if not re.fullmatch(r"[0-9A-Za-z:\-]{1,64}", event_id) or not hub.tracker.get(event_id):
            raise HTTPException(404)
        body = await request.json()
        data = str(body.get("data", ""))
        if not data.startswith("data:image/jpeg;base64,") or len(data) > 400_000:
            raise HTTPException(400, "Expected a JPEG data URL under 300 KB")
        name = event_id.replace(":", "_") + ".jpg"
        (THUMBS_DIR / name).write_bytes(base64.b64decode(data.split(",", 1)[1]))
        url = "/thumbs/" + name
        hub.tracker.set_thumb(event_id, url)
        await hub.broadcast({"type": "events", "events": [hub.tracker.get(event_id)]})
        return {"thumb": url}

    if args.demo:
        @app.get("/demo/wildfires.png")
        async def demo_poster() -> FileResponse:
            return FileResponse(ROOT / "resources" / "wildfires.png")

    app.mount("/thumbs", StaticFiles(directory=str(THUMBS_DIR)), name="thumbs")
    app.mount("/", StaticFiles(directory=str(WEB_DIR), html=True), name="web")
    return app


def main() -> None:
    ap = argparse.ArgumentParser(description="Hermes web UI backend")
    ap.add_argument("--host", default="127.0.0.1", help="use 0.0.0.0 to open the UI to your network")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--token", default=None, help="require ?token=... for the UI and API")
    ap.add_argument("--redis-host", default="127.0.0.1")
    ap.add_argument("--redis-port", type=int, default=6379)
    ap.add_argument("--drone", action="store_true", help="connect to a Ryze Tello (join its Wi-Fi first)")
    ap.add_argument("--no-relay", action="store_true", help="don't relay the Tello camera to RTSP")
    ap.add_argument("--drone-uri", default=DEFAULT_DRONE_URI, help="pipeline source URI that is the drone")
    ap.add_argument("--whep-port", type=int, default=8889, help="MediaMTX WebRTC port")
    ap.add_argument("--whep-path", default="/hermes/whep")
    ap.add_argument("--demo", action="store_true", help="simulated pipeline + drone for UI development")
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(levelname)s %(message)s")
    DATA_DIR.mkdir(exist_ok=True)
    uvicorn.run(create_app(args), host=args.host, port=args.port, log_level="warning")


if __name__ == "__main__":
    main()
