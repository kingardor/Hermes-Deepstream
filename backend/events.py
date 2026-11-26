"""Turn per-frame detections into fire events and a per-source timeline."""
import json
import sqlite3
import threading
import time
from typing import Any, Dict, List, Optional

MIN_HITS = 3            # frames before a track becomes an event (filters single-frame blips)
GAP_S = 3.0             # unseen longer than this = a new "appearance" of the same track
NEW_S = 30.0            # an event is NEW for its first 30 s
ENDED_S = 60.0          # unseen this long = ENDED
TIMELINE_KEEP_S = 30 * 60
TIMELINE_MERGE_S = 2.0  # merge presence intervals closer than this


class FireEvent:
    def __init__(self, src: int, track: int, now: float, run: str = "") -> None:
        self.id = "%s%d-%d" % (run, src, track)
        self.src = src
        self.track = track
        self.first_seen = now
        self.last_seen = now
        self.peak_conf = 0.0
        self.hits = 0
        self.appearances = 1
        self.box: List[int] = [0, 0, 0, 0]
        self.acked_at: Optional[float] = None
        self.thumb: Optional[str] = None

    def status(self, now: float) -> str:
        if self.acked_at:
            return "acknowledged"
        if now - self.last_seen > ENDED_S:
            return "ended"
        if self.appearances >= 2:
            return "intermittent"
        if now - self.first_seen < NEW_S:
            return "new"
        return "active"

    def to_dict(self, now: float) -> Dict[str, Any]:
        return {
            "id": self.id, "src": self.src, "track": self.track,
            "first_seen": self.first_seen, "last_seen": self.last_seen,
            "peak_conf": round(self.peak_conf, 3), "hits": self.hits,
            "appearances": self.appearances, "box": self.box,
            "acked_at": self.acked_at, "thumb": self.thumb,
            "status": self.status(now), "visible": now - self.last_seen <= GAP_S,
        }


class EventTracker:
    def __init__(self, db_path: Optional[str] = None) -> None:
        self._lock = threading.Lock()
        self._events: Dict[str, FireEvent] = {}
        self._pending: Dict[str, FireEvent] = {}          # tracks below MIN_HITS
        self._timeline: Dict[int, List[List[float]]] = {}   # src -> [[start, end], ...]
        self._db: Optional[sqlite3.Connection] = None
        # Tracker ids restart with the pipeline; prefix them with its start time so runs don't collide
        self.run = ""
        if db_path:
            self._db = sqlite3.connect(db_path, check_same_thread=False)
            self._db.execute("CREATE TABLE IF NOT EXISTS events (id TEXT PRIMARY KEY, data TEXT, updated REAL)")
            self._load()

    # -- ingestion ----------------------------------------------------------
    def ingest(self, msg: Dict[str, Any]) -> List[str]:
        """Feed one frame message from the pipeline. Returns ids of events that changed."""
        now = msg.get("t", time.time() * 1000) / 1000.0
        src = int(msg.get("src", 0))
        changed: List[str] = []
        objs = msg.get("objs") or []
        with self._lock:
            if objs:
                self._mark_present(src, now)
            for o in objs:
                key = "%s%d-%d" % (self.run, src, int(o["id"]))
                ev = self._events.get(key) or self._pending.get(key)
                if ev is None:
                    ev = FireEvent(src, int(o["id"]), now, self.run)
                    self._pending[key] = ev
                if now - ev.last_seen > GAP_S:
                    ev.appearances += 1
                ev.last_seen = now
                ev.hits += 1
                conf = float(o.get("conf", 0))
                if conf >= 0:  # tracker-only frames report a negative confidence
                    ev.peak_conf = max(ev.peak_conf, conf)
                ev.box = [int(o.get("x", 0)), int(o.get("y", 0)), int(o.get("bw", 0)), int(o.get("bh", 0))]
                if key in self._pending and ev.hits >= MIN_HITS:
                    self._events[key] = self._pending.pop(key)
                    changed.append(key)
                elif key in self._events and (ev.appearances > 1 or ev.hits % 30 == 0):
                    changed.append(key)
        return changed

    def _mark_present(self, src: int, now: float) -> None:
        lanes = self._timeline.setdefault(src, [])
        if lanes and now - lanes[-1][1] <= TIMELINE_MERGE_S:
            lanes[-1][1] = now
        else:
            lanes.append([now, now])
        cutoff = now - TIMELINE_KEEP_S
        while lanes and lanes[0][1] < cutoff:
            lanes.pop(0)

    # -- operator actions -----------------------------------------------------
    def acknowledge(self, event_id: str) -> Optional[Dict[str, Any]]:
        with self._lock:
            ev = self._events.get(event_id)
            if not ev:
                return None
            ev.acked_at = time.time()
            self._save(ev)
            return ev.to_dict(time.time())

    def set_thumb(self, event_id: str, url: str) -> bool:
        with self._lock:
            ev = self._events.get(event_id)
            if not ev:
                return False
            ev.thumb = url
            self._save(ev)
            return True

    # -- reads ------------------------------------------------------------------
    def snapshot(self) -> List[Dict[str, Any]]:
        now = time.time()
        with self._lock:
            evs = [e.to_dict(now) for e in self._events.values()]
        return sorted(evs, key=lambda e: e["first_seen"], reverse=True)

    def get(self, event_id: str) -> Optional[Dict[str, Any]]:
        with self._lock:
            ev = self._events.get(event_id)
            return ev.to_dict(time.time()) if ev else None

    def timeline(self) -> Dict[str, List[List[float]]]:
        with self._lock:
            return {str(k): [list(i) for i in v] for k, v in self._timeline.items()}

    def flush(self) -> None:
        """Persist changed events (called periodically, not per frame)."""
        with self._lock:
            for ev in self._events.values():
                self._save(ev)

    # -- persistence ------------------------------------------------------------
    def _save(self, ev: FireEvent) -> None:
        if not self._db:
            return
        data = json.dumps({k: v for k, v in ev.__dict__.items()})
        self._db.execute("INSERT OR REPLACE INTO events (id, data, updated) VALUES (?, ?, ?)",
                         (ev.id, data, time.time()))
        self._db.commit()

    def _load(self) -> None:
        assert self._db is not None
        cutoff = time.time() - 24 * 3600
        for _id, data in self._db.execute("SELECT id, data FROM events WHERE updated > ?", (cutoff,)):
            d = json.loads(data)
            ev = FireEvent(d["src"], d["track"], d["first_seen"])  # id restored below
            ev.__dict__.update(d)
            self._events[ev.id] = ev
