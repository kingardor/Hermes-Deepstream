"""Device health for the UI: GPU load, SoC temperature, memory. Works on any Linux;
uses Jetson sysfs paths when present. Returns None for anything it can't read."""
import glob
import os
from typing import Any, Dict, Optional

GPU_LOAD_PATHS = [
    "/sys/devices/gpu.0/load",                         # Nano / TX2
    "/sys/devices/17000000.gv11b/load",                # Xavier NX / AGX Xavier
    "/sys/devices/platform/17000000.gv11b/load",
    "/sys/devices/platform/gpu.0/load",
]


def _read(path: str) -> Optional[str]:
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return None


def gpu_load() -> Optional[int]:
    for p in GPU_LOAD_PATHS:
        v = _read(p)
        if v and v.isdigit():
            return round(int(v) / 10)  # reported in 0..1000
    return None


def soc_temp() -> Optional[float]:
    temps = []
    for zone in glob.glob("/sys/class/thermal/thermal_zone*"):
        v = _read(os.path.join(zone, "temp"))
        name = (_read(os.path.join(zone, "type")) or "").lower()
        if v and v.lstrip("-").isdigit() and "pmic" not in name:
            t = int(v) / 1000.0
            if 0 < t < 150:
                temps.append(t)
    return round(max(temps), 1) if temps else None


def memory_used() -> Optional[int]:
    info = {}
    for line in (_read("/proc/meminfo") or "").splitlines():
        k, _, rest = line.partition(":")
        info[k] = int(rest.split()[0]) if rest.split() else 0
    if "MemTotal" in info and "MemAvailable" in info and info["MemTotal"]:
        return round(100 * (1 - info["MemAvailable"] / info["MemTotal"]))
    return None


def device() -> Dict[str, Any]:
    return {"gpu": gpu_load(), "temp": soc_temp(), "mem": memory_used()}
