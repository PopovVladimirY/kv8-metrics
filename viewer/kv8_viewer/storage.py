"""
In-memory time-series store for the kv8-viewer service.

Aggregated metric records are kept in a ring-buffer per metric name,
allowing the REST API to query recent values efficiently.
"""

import threading
import time
from collections import defaultdict, deque
from dataclasses import dataclass, field
from typing import Dict, List, Optional


@dataclass
class StoredMetric:
    """A persisted aggregated metric snapshot."""

    name: str
    metric_type: str
    window_start: float
    window_end: float
    count: int
    sum: float
    min: float
    max: float
    mean: float
    tags: Dict[str, str] = field(default_factory=dict)


class MetricStore:
    """
    Thread-safe in-memory store for recent aggregated metrics.

    Parameters
    ----------
    max_points_per_metric:
        Maximum number of data points retained per metric name.
    retention_seconds:
        Entries older than this age (seconds) are pruned on insertion.
    """

    def __init__(
        self,
        max_points_per_metric: int = 1440,
        retention_seconds: float = 86400.0,
    ) -> None:
        self._max = max_points_per_metric
        self._retention = retention_seconds
        self._lock = threading.Lock()
        self._data: Dict[str, deque] = defaultdict(
            lambda: deque(maxlen=self._max)
        )

    def record(self, metric: Dict) -> None:
        """Insert one aggregated metric snapshot."""
        entry = StoredMetric(
            name=metric["name"],
            metric_type=metric.get("type", "gauge"),
            window_start=float(metric.get("window_start", time.time())),
            window_end=float(metric.get("window_end", time.time())),
            count=int(metric.get("count", 0)),
            sum=float(metric.get("sum", 0)),
            min=float(metric.get("min", 0)),
            max=float(metric.get("max", 0)),
            mean=float(metric.get("mean", 0)),
            tags=metric.get("tags", {}),
        )
        cutoff = time.time() - self._retention
        with self._lock:
            q = self._data[entry.name]
            # prune old entries
            while q and q[0].window_end < cutoff:
                q.popleft()
            q.append(entry)

    def query(
        self,
        name: str,
        since: Optional[float] = None,
        until: Optional[float] = None,
    ) -> List[StoredMetric]:
        """Return stored points for *name* within the given time range."""
        now = time.time()
        since = since or (now - 3600)
        until = until or now
        with self._lock:
            return [
                p
                for p in self._data.get(name, [])
                if since <= p.window_end <= until
            ]

    def list_metrics(self) -> List[str]:
        """Return all known metric names."""
        with self._lock:
            return sorted(self._data.keys())

    def latest(self, name: str) -> Optional[StoredMetric]:
        """Return the most recent snapshot for *name*, or ``None``."""
        with self._lock:
            q = self._data.get(name)
            return q[-1] if q else None
