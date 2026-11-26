"""Demo pipeline for developing the UI without a Jetson.

Replays the fires visible in resources/wildfires.png (real Hermes output, 2x2 tiles)
as detection messages in the same format the DeepStream app publishes to Redis.
Boxes are in per-source muxer coordinates (1920x1080), like the real app.
"""
import math
import random
import time
from typing import Any, Callable, Dict, List, Tuple

PIPELINE = {
    "sources": [
        "file:///home/hermes/videos/ridge-north.mp4",
        "rtsp://tower-07.local:554/stream",
        "rtsp://127.0.0.1:6969/hermes",
        "file:///home/hermes/videos/valley-east.mp4",
    ],
    "rows": 2, "columns": 2, "width": 1920, "height": 1080,
    "tile_width": 1920, "tile_height": 1080, "rtsp": "",
}

# (source, track id, box x/y/w/h, start offset s, visibility pattern)
# Offsets are relative to "now" when the demo starts; negative = already burning.
TRACKS: List[Tuple[int, int, Tuple[int, int, int, int], float, str]] = [
    (3, 2, (530, 360, 1056, 563), -470, "steady"),
    (3, 18, (4, 546, 152, 89), -300, "steady"),
    (0, 16, (0, 201, 1920, 603), -280, "steady"),
    (1, 12, (1164, 810, 169, 133), -150, "intermittent"),
    (2, 13, (714, 614, 180, 121), -20, "steady"),
    (2, 14, (845, 832, 70, 32), -12, "steady"),
]
ACKNOWLEDGED = ["3-2"]


def _visible(pattern: str, age: float) -> bool:
    if age < 0:
        return False
    if pattern == "intermittent":
        return (age % 40) < 7   # flickers in for 7 s every 40 s
    return True


def frame(src: int, t: float, start: float) -> Dict[str, Any]:
    objs = []
    for s, track, (x, y, w, h), offset, pattern in TRACKS:
        if s != src or not _visible(pattern, t - (start + offset)):
            continue
        wobble = int(6 * math.sin(t * 2 + track))
        conf = round(min(0.97, 0.55 + 0.35 * abs(math.sin(t / 7 + track)) + random.uniform(-0.04, 0.04)), 3)
        if pattern == "intermittent":
            conf = round(conf * 0.7, 3)
        objs.append({"id": track, "cls": "fire", "conf": conf,
                     "x": max(0, x + wobble), "y": max(0, y - wobble), "bw": w, "bh": h})
    fps = 30 if src == 2 else 31
    return {"t": int(t * 1000), "src": src, "frame": int(t * fps), "fps": fps,
            "w": 1920, "h": 1080, "objs": objs}


def seed(ingest: Callable[[Dict[str, Any]], Any], start: float) -> None:
    """Back-fill the last 8 minutes so the timeline and events have history."""
    t = start - 480
    while t < start:
        for src in range(4):
            ingest(frame(src, t, start))
        t += 0.5
