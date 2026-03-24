"""
Metric types for kv8-metrics client library.

Provides Counter, Gauge, Histogram, and Timer metric primitives.
All metric types are thread-safe.
"""

import math
import threading
import time
from dataclasses import dataclass, field
from typing import Dict, List, Optional


@dataclass
class MetricPoint:
    """A single metric observation snapshot."""

    name: str
    value: float
    metric_type: str
    timestamp: float = field(default_factory=time.time)
    tags: Dict[str, str] = field(default_factory=dict)

    def to_dict(self) -> Dict:
        return {
            "name": self.name,
            "value": self.value,
            "type": self.metric_type,
            "timestamp": self.timestamp,
            "tags": self.tags,
        }


class Counter:
    """
    A monotonically increasing counter.

    Suitable for counting events such as requests served,
    tasks completed, or errors occurred.
    """

    def __init__(self, name: str, tags: Optional[Dict[str, str]] = None) -> None:
        self.name = name
        self.tags: Dict[str, str] = tags or {}
        self._value: float = 0.0
        self._lock = threading.Lock()

    def increment(self, amount: float = 1.0) -> None:
        """Increment the counter by *amount* (must be non-negative)."""
        if amount < 0:
            raise ValueError("Counter can only be incremented by a non-negative amount")
        with self._lock:
            self._value += amount

    @property
    def value(self) -> float:
        with self._lock:
            return self._value

    def snapshot(self) -> MetricPoint:
        """Return a point-in-time snapshot of this metric."""
        with self._lock:
            return MetricPoint(
                name=self.name,
                value=self._value,
                metric_type="counter",
                tags=dict(self.tags),
            )


class Gauge:
    """
    A gauge that represents an arbitrarily fluctuating value.

    Suitable for measurements such as memory usage, active connections,
    or queue depth.
    """

    def __init__(self, name: str, tags: Optional[Dict[str, str]] = None) -> None:
        self.name = name
        self.tags: Dict[str, str] = tags or {}
        self._value: float = 0.0
        self._lock = threading.Lock()

    def set(self, value: float) -> None:
        """Set the gauge to an absolute *value*."""
        with self._lock:
            self._value = value

    def increment(self, amount: float = 1.0) -> None:
        """Increment the gauge by *amount*."""
        with self._lock:
            self._value += amount

    def decrement(self, amount: float = 1.0) -> None:
        """Decrement the gauge by *amount*."""
        with self._lock:
            self._value -= amount

    @property
    def value(self) -> float:
        with self._lock:
            return self._value

    def snapshot(self) -> MetricPoint:
        with self._lock:
            return MetricPoint(
                name=self.name,
                value=self._value,
                metric_type="gauge",
                tags=dict(self.tags),
            )


class Histogram:
    """
    Samples observations and tracks their distribution.

    Suitable for measuring request durations, payload sizes, etc.
    Exposes count, sum, min, max and approximate percentiles.
    """

    # Default percentile buckets (upper bounds in seconds for latency, or raw)
    DEFAULT_BUCKETS: List[float] = [
        0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0,
    ]

    def __init__(
        self,
        name: str,
        tags: Optional[Dict[str, str]] = None,
        buckets: Optional[List[float]] = None,
    ) -> None:
        self.name = name
        self.tags: Dict[str, str] = tags or {}
        self._buckets: List[float] = sorted(buckets or self.DEFAULT_BUCKETS)
        self._lock = threading.Lock()
        self._count: int = 0
        self._sum: float = 0.0
        self._min: float = math.inf
        self._max: float = -math.inf
        self._bucket_counts: List[int] = [0] * len(self._buckets)

    def observe(self, value: float) -> None:
        """Record a single observation."""
        with self._lock:
            self._count += 1
            self._sum += value
            if value < self._min:
                self._min = value
            if value > self._max:
                self._max = value
            for i, bound in enumerate(self._buckets):
                if value <= bound:
                    self._bucket_counts[i] += 1

    @property
    def count(self) -> int:
        with self._lock:
            return self._count

    @property
    def mean(self) -> float:
        with self._lock:
            return self._sum / self._count if self._count > 0 else 0.0

    def snapshot(self) -> MetricPoint:
        """Return the current mean as a MetricPoint.

        The full distribution detail is embedded in *tags*.
        """
        with self._lock:
            count = self._count
            mean = self._sum / count if count > 0 else 0.0
            min_val = self._min if count > 0 else 0.0
            max_val = self._max if count > 0 else 0.0
            bucket_str = ",".join(
                f"{b}:{c}" for b, c in zip(self._buckets, self._bucket_counts)
            )
        extra_tags = {
            "count": str(count),
            "min": str(min_val),
            "max": str(max_val),
            "buckets": bucket_str,
        }
        tags = {**self.tags, **extra_tags}
        return MetricPoint(
            name=self.name,
            value=mean,
            metric_type="histogram",
            tags=tags,
        )


class Timer:
    """
    Convenience wrapper around :class:`Histogram` for measuring durations.

    Can be used as a context manager::

        with collector.timer("db.query_time"):
            result = db.execute(query)
    """

    def __init__(self, name: str, tags: Optional[Dict[str, str]] = None) -> None:
        self.name = name
        self._histogram = Histogram(name, tags=tags)
        self._start: Optional[float] = None

    def start(self) -> None:
        self._start = time.monotonic()

    def stop(self) -> float:
        """Stop the timer and return elapsed seconds."""
        if self._start is None:
            raise RuntimeError("Timer was not started")
        elapsed = time.monotonic() - self._start
        self._histogram.observe(elapsed)
        self._start = None
        return elapsed

    def __enter__(self) -> "Timer":
        self.start()
        return self

    def __exit__(self, *_) -> None:
        self.stop()

    @property
    def count(self) -> int:
        return self._histogram.count

    def snapshot(self) -> MetricPoint:
        snap = self._histogram.snapshot()
        snap.metric_type = "timer"
        return snap
