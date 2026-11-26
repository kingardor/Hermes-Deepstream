# Hermes App

Wildfires have been ever-increasing, devouring our planet earth, rendering our planet worse day by day. With early detection and mitigation, it is possible to reduce the damage caused by wildfires.

![Hermes live ops console: four camera feeds with fire detections, fire events, a detection timeline and device health](docs/screenshots/live-console.jpg)

To better enable our front-line workers, here is Hermes, an AI-powered Computer Vision application that helps in early detection of Wildfires using reconnaissance drones.

Hermes now comes with a **web UI**: a live ops console for every camera and a flight mode for the drone, usable from any browser on the network.

## Citations

* [AlexeyAB/darknet](https://github.com/AlexeyAB/darknet)
* [damiafuentes/DJITelloPy](https://github.com/damiafuentes/DJITelloPy)
* [bluenviron/mediamtx](https://github.com/bluenviron/mediamtx)
* [tiangolo/fastapi](https://github.com/tiangolo/fastapi)

## Index

1. [Introduction](#introduction)
2. [The web UI](#the-web-ui)
3. [Deepstream Setup](#deepstream-setup)
4. [Web UI Setup](#web-ui-setup)
5. [Ryze Tello Setup](#ryze-tello-setup)
6. [Running the Application](#running-the-application)
7. [Trying the UI without a Jetson](#trying-the-ui-without-a-jetson)
8. [Reference](#reference)

## Introduction

Hermes Application consists of two parts. An Intelligent Video Analytics Pipeline powered by Deepstream and NVIDIA Jetson Xavier NX and a reconnaissance drone, for which I have used a Ryze Tello.

![Tello and Jetson NX](resources/tellonx.jpg)

This project is a proof-of-concept, trying to show that surveillance and mapping of wildfires can be done with a drone and an onboard Jetson platform.

## The web UI

### Live: ops console

- **Video wall** with every input source. A tile gets a red outline while it has an active fire; click a tile to focus it.
- **Fire events**, one per tracked fire (from the DCF tracker's IDs): a snapshot grabbed from the live video, source, when it was first seen, and peak confidence. Events move through **New → Active → Intermittent → Ended**, and an operator can **Acknowledge** them.
- **Detection timeline** with a lane per source, showing when fire was present, with first-seen markers, a 5/10/30-minute window and Pause.
- **Health**: pipeline state (running / stalled / offline), FPS per source, and the Jetson's GPU load, temperature and memory.

### Flight: drone control

![Hermes flight mode](docs/screenshots/flight-mode.jpg)

- The drone's feed, large, with a minimal HUD (altitude, yaw, speed) and a centre reticle.
- **Fire guidance**: works out which side of the frame the biggest fire is on and suggests the rotation that centres it (using the Tello's ~83° field of view).
- **Telemetry** from the Tello: battery, flight time, height and temperature.
- **Controls**: an arming switch, Take off / Land, a separate **Emergency stop**, on-screen pads (30 cm / 30° steps), keyboard (`W A S D`, arrows, `T`, `Space`) and gamepad sticks.

**Safety built in:**

- Nothing moves until the controls are armed.
- Commands run one at a time.
- Emergency stop always works and jumps the queue.
- A keepalive stops the Tello from auto-landing after 15 s of hovering.
- If every operator's browser disconnects, the drone **lands and disarms automatically** after 10 s.

<sub>The screenshots use the built-in demo data: the replayed detections are taken from `resources/wildfires.png`. The live console screenshot went through the real video path (RTSP → MediaMTX → WebRTC).</sub>

### How it fits together

```text
 cameras / drone ──▶ hermes-app (DeepStream) ──┬─▶ local display (optional)
                     YOLOv3 + DCF tracker       ├─▶ RTSP :8555/hermes ──▶ MediaMTX ──WebRTC──▶ browser
                                                └─▶ Redis  hermes:detections ──▶ backend ──WebSocket──▶ browser
 Tello ◀──── commands / telemetry ──── backend (FastAPI :8080, serves the UI) ◀──── browser
 Tello camera ──▶ backend relay rtsp://127.0.0.1:6969/hermes ──▶ hermes-app (as a source)
```

- **hermes-app** publishes every frame's detections (source, track ID, confidence, box) to Redis, and writes its source list and tiler layout to `hermes:pipeline`. It also serves the annotated, tiled output as RTSP.
- **MediaMTX** turns that RTSP into WebRTC for browsers (sub-second latency).
- **backend/** turns detections into fire events, tracks health, owns the drone, and pushes everything to the UI over one WebSocket.
- **web/** is plain HTML, CSS and JS; there's no build step. Each tile is a crop of the single tiled stream.

## Deepstream Setup

This post assumes you have a fully functional Jetson device. If not, you can refer the documentation [here](https://docs.nvidia.com/jetson/jetpack/install-jetpack/index.html).

### 1. Install System Dependencies

```sh
sudo apt install \
libssl1.0.0 \
libgstreamer1.0-0 \
gstreamer1.0-tools \
gstreamer1.0-plugins-good \
gstreamer1.0-plugins-bad \
gstreamer1.0-plugins-ugly \
gstreamer1.0-libav \
libgstrtspserver-1.0-0 \
libgstrtspserver-1.0-dev \
libhiredis-dev \
libboost-filesystem-dev \
libjansson4=2.11-1
```

`libgstrtspserver-1.0-dev` and `libhiredis-dev` are needed to build the RTSP output and the Redis publisher.

### 2. Install Deepstream

Download the DeepStream 5.1 Jetson Debian package `deepstream-5.1_5.1.0-1_arm64.deb`, to the Jetson device from [here](https://developer.nvidia.com/deepstream-getting-started). Then enter the command:

```sh
sudo apt install deepstream-5.1_5.1.0-1_arm64.deb
```

## Web UI Setup

### 1. Redis

Redis carries detections from the DeepStream app to the web UI backend.

```sh
sudo apt install redis-server
```

### 2. MediaMTX

MediaMTX re-streams the pipeline's RTSP output to browsers over WebRTC. Download the `linux_arm64` (or `linux_arm64v8`) build from the [MediaMTX releases](https://github.com/bluenviron/mediamtx/releases) and keep the `mediamtx` binary next to this repo's [`mediamtx.yml`](mediamtx.yml).

Browsers need TCP **8889** and UDP **8189** to reach the Jetson. If they're on another network, set `webrtcAdditionalHosts` in `mediamtx.yml` to the Jetson's IP.

### 3. Backend

The backend needs Python 3.8 or newer. The Tello camera relay also needs the GStreamer Python bindings (PyGObject).

**JetPack 5 / Ubuntu 20.04**, where Python 3.8 is the system Python:

```sh
sudo apt install python3-venv python3-gi gir1.2-gst-rtsp-server-1.0
python3 -m venv --system-site-packages .venv
.venv/bin/pip install -r backend/requirements.txt
```

**JetPack 4.x / Ubuntu 18.04**, where the apt `python3-gi` is built for Python 3.6, so install PyGObject with pip instead:

```sh
sudo apt install python3.8 python3.8-venv python3.8-dev \
  libgirepository1.0-dev libcairo2-dev gir1.2-gst-rtsp-server-1.0
python3.8 -m venv .venv
.venv/bin/pip install -r backend/requirements.txt PyGObject
```

Without `--drone`, the backend doesn't need PyGObject at all.

## Ryze Tello Setup

### 1. Connect Tello

First, connect the Jetson Device to the WiFi network of Tello.

![Ryze Tello WiFi](resources/dronewifi.png)

The backend connects to the Tello itself when started with `--drone`. To check connectivity by hand:

```python
from djitellopy import Tello
tello = Tello()
tello.connect()
print(tello.get_battery())
```

If you get `Timeout exceed on command command`, check your Wi-Fi connection to the drone.

## Running the Application

### 1. Clone the repository

```sh
sudo apt install git git-lfs
git clone https://github.com/kingardor/Hermes-Deepstream.git
cd Hermes-Deepstream
git lfs install
git lfs pull
```

### 2. Build

```sh
make clean && make -j$(nproc)
```

This builds `hermes-app` and the custom YOLOv3 parser. You need to do it again only after source changes.

### 3. Choose your inputs

Create `inputsources.txt` with one video file or RTSP URL per line. Blank lines and lines starting with `#` are ignored.

```sh
file:///home/astr1x/Videos/Wildfire1.mp4
rtsp://admin:admin%40123@192.168.1.1:554/stream
# the drone, relayed by the backend:
rtsp://127.0.0.1:6969/hermes
```

Stream credentials never reach the browser; the UI shows URLs without them.

### 4. Start everything

Start these in four terminals, in this order:

```sh
# 1. Web UI backend (add --drone to fly the Tello; it also relays the Tello camera to RTSP)
.venv/bin/python backend/hermes_backend.py --host 0.0.0.0 --drone

# 2. MediaMTX
./mediamtx mediamtx.yml

# 3. The DeepStream app (--no-display to run without the local window)
./hermes-app

# 4. Open the UI from any machine on the network
xdg-open http://<jetson-ip>:8080
```

The first run builds the TensorRT engine (`model_b<N>_gpu0_fp32.engine`), which takes a few minutes.

The UI comes up straight away and shows "Waiting for the pipeline" until `hermes-app` publishes its sources.

> [!IMPORTANT]
> With `--host 0.0.0.0`, anyone on the network can open the UI, and that includes the drone controls. Add `--token <secret>` to the backend and open the UI as `http://<jetson-ip>:8080/?token=<secret>`.

### 5. Fly

1. Open **Flight**.
2. Switch **Armed** on.
3. Take off with the button, `T`, or gamepad **A**.
4. Move with `W A S D` / the left stick, altitude with `↑ ↓`, and rotate with `← →` / the right stick.
5. Land with **Land**, `Space`, or **B**.

**Emergency stop** cuts the motors immediately, even mid-move. It has no keyboard shortcut on purpose.

## Trying the UI without a Jetson

The backend has a demo mode that replays the fires in `resources/wildfires.png` as live detections and simulates a Tello. It needs no DeepStream, Redis, MediaMTX or drone:

```sh
python3 -m venv .venv && .venv/bin/pip install -r backend/requirements.txt
.venv/bin/python backend/hermes_backend.py --demo
# open http://127.0.0.1:8080
```

## Reference

### hermes-app options

| Option | Default | |
|---|---|---|
| `--no-display` | off | No local window (headless) |
| `--no-rtsp` | off | Don't serve RTSP (disables video in the web UI) |
| `--rtsp-port` | `8555` | RTSP output port (`rtsp://<host>:8555/hermes`) |
| `--redis-host` / `--redis-port` | `127.0.0.1` / `6379` | Where to publish detections |

### Backend options

| Option | Default | |
|---|---|---|
| `--host` / `--port` | `127.0.0.1` / `8080` | Where to serve the UI |
| `--token` | none | Require `?token=` for the UI and API |
| `--drone` | off | Connect to a Tello and relay its camera to `rtsp://127.0.0.1:6969/hermes` |
| `--drone-uri` | `rtsp://127.0.0.1:6969/hermes` | Which pipeline source is the drone |
| `--whep-port` / `--whep-path` | `8889` / `/hermes/whep` | MediaMTX WebRTC endpoint |
| `--demo` | off | Simulated pipeline and drone |

Fire events are kept in `data/events.db` for 24 hours, and snapshots in `data/thumbs/`.

### What changed from the original

- **New:** a web UI (Live console + Flight mode), a FastAPI backend, RTSP output, and a Redis detection publisher in `hermes-app`.
- **Drone control moved into the backend.** `tello-control.py` is replaced by `backend/drone.py`.
- **Fixed: the drone's yaw keys were swapped** (left arrow turned the drone right).
- **Fixed: the keyboard listener captured keys system-wide.** Controls now work only in the Flight tab, and only while armed.
- **Fixed: the stream muxer's batch timeout was 4 s instead of the intended 40 ms**, which stalled every source whenever one lagged.
- **Fixed: `models/YOLOv3WildFires/labels.txt` was empty.** It now holds `fire`.
- **Cleanup:**
  - Drone telemetry (`get_status()`) was never shown; it now feeds the UI.
  - Removed the unused libcurl and OpenCV dependencies.
  - `inputsources.txt` now accepts blank lines and comments.
  - Removed stale names from the code.
- **Upgrade:** djitellopy 2.4, which reads telemetry from the Tello's state packets instead of a blocking query per value.

### Limits

- The Tello has no GPS, so fires can't be placed on a map. That needs a GPS-equipped drone or fixed cameras at known positions.
- Fire boxes are burned into the video by DeepStream's on-screen display; the UI doesn't draw them itself.
