"""
MetricsCollector — central registry for all metric instances.

Usage::

    from kv8_client import MetricsCollector

    collector = MetricsCollector(
        service="my-service",
        kafka_bootstrap_servers="localhost:9092",
    )

    requests = collector.counter("http.requests", tags={"endpoint": "/api/v1"})
    latency  = collector.histogram("http.latency_seconds")
    active   = collector.gauge("active_connections")

    collector.start()          # start background flush thread
    # ... application logic
    requests.increment()
    with latency as t:
        do_work()
    collector.stop()           # flush remaining metrics and stop
"""

import logging
import threading
import time
from typing import Dict, List, Optional

from .metrics import Counter, Gauge, Histogram, MetricPoint, Timer
from .producer import KafkaMetricsProducer

logger = logging.getLogger(__name__)


class MetricsCollector:
    """
    Thread-safe registry and flush manager for metrics.

    Parameters
    ----------
    service:
        Name of the service that owns these metrics.  Added as a ``service``
        tag on every metric that does not already carry one.
    kafka_bootstrap_servers:
        Comma-separated list of ``host:port`` pairs for Kafka brokers.
    topic:
        Kafka topic to publish raw metric snapshots to.
        Defaults to ``kv8.metrics.raw``.
    flush_interval_seconds:
        How often (in seconds) to flush accumulated metrics to Kafka.
        Defaults to ``10``.
    global_tags:
        Additional key/value tags added to every metric.
    producer:
        Optional pre-built :class:`~kv8_client.producer.KafkaMetricsProducer`
        for testing or custom configurations.
    """

    def __init__(
        self,
        service: str,
        kafka_bootstrap_servers: str = "localhost:9092",
        topic: str = "kv8.metrics.raw",
        flush_interval_seconds: float = 10.0,
        global_tags: Optional[Dict[str, str]] = None,
        producer: Optional[KafkaMetricsProducer] = None,
    ) -> None:
        self._service = service
        self._topic = topic
        self._flush_interval = flush_interval_seconds
        self._global_tags: Dict[str, str] = {"service": service, **(global_tags or {})}

        self._counters:   Dict[str, Counter]   = {}
        self._gauges:     Dict[str, Gauge]     = {}
        self._histograms: Dict[str, Histogram] = {}
        self._timers:     Dict[str, Timer]     = {}
        self._registry_lock = threading.Lock()

        self._producer = producer or KafkaMetricsProducer(
            bootstrap_servers=kafka_bootstrap_servers,
            topic=topic,
        )

        self._flush_thread: Optional[threading.Thread] = None
        self._stop_event = threading.Event()

    # ------------------------------------------------------------------
    # Metric factory helpers
    # ------------------------------------------------------------------

    def counter(self, name: str, tags: Optional[Dict[str, str]] = None) -> Counter:
        """Return (or create) a :class:`~kv8_client.metrics.Counter`."""
        key = self._key(name, tags)
        with self._registry_lock:
            if key not in self._counters:
                self._counters[key] = Counter(name, tags=self._merged_tags(tags))
            return self._counters[key]

    def gauge(self, name: str, tags: Optional[Dict[str, str]] = None) -> Gauge:
        """Return (or create) a :class:`~kv8_client.metrics.Gauge`."""
        key = self._key(name, tags)
        with self._registry_lock:
            if key not in self._gauges:
                self._gauges[key] = Gauge(name, tags=self._merged_tags(tags))
            return self._gauges[key]

    def histogram(self, name: str, tags: Optional[Dict[str, str]] = None) -> Histogram:
        """Return (or create) a :class:`~kv8_client.metrics.Histogram`."""
        key = self._key(name, tags)
        with self._registry_lock:
            if key not in self._histograms:
                self._histograms[key] = Histogram(name, tags=self._merged_tags(tags))
            return self._histograms[key]

    def timer(self, name: str, tags: Optional[Dict[str, str]] = None) -> Timer:
        """Return (or create) a :class:`~kv8_client.metrics.Timer`."""
        key = self._key(name, tags)
        with self._registry_lock:
            if key not in self._timers:
                self._timers[key] = Timer(name, tags=self._merged_tags(tags))
            return self._timers[key]

    # ------------------------------------------------------------------
    # Lifecycle
    # ------------------------------------------------------------------

    def start(self) -> None:
        """Start the background flush thread."""
        if self._flush_thread and self._flush_thread.is_alive():
            return
        self._stop_event.clear()
        self._flush_thread = threading.Thread(
            target=self._flush_loop,
            name="kv8-metrics-flush",
            daemon=True,
        )
        self._flush_thread.start()
        logger.info(
            "MetricsCollector started (service=%s, topic=%s, interval=%.1fs)",
            self._service,
            self._topic,
            self._flush_interval,
        )

    def stop(self) -> None:
        """Stop the flush thread and flush any remaining metrics."""
        self._stop_event.set()
        if self._flush_thread:
            self._flush_thread.join(timeout=self._flush_interval + 5)
        self.flush()
        self._producer.close()
        logger.info("MetricsCollector stopped (service=%s)", self._service)

    def flush(self) -> int:
        """
        Immediately snapshot all registered metrics and send them to Kafka.

        Returns the number of metric points published.
        """
        points = self._collect_snapshots()
        if points:
            self._producer.publish(points)
        return len(points)

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------

    def _flush_loop(self) -> None:
        while not self._stop_event.is_set():
            self._stop_event.wait(self._flush_interval)
            if not self._stop_event.is_set():
                try:
                    count = self.flush()
                    logger.debug("Flushed %d metric points", count)
                except Exception:
                    logger.exception("Error during metrics flush")

    def _collect_snapshots(self) -> List[MetricPoint]:
        points: List[MetricPoint] = []
        with self._registry_lock:
            for m in self._counters.values():
                points.append(m.snapshot())
            for m in self._gauges.values():
                points.append(m.snapshot())
            for m in self._histograms.values():
                points.append(m.snapshot())
            for m in self._timers.values():
                points.append(m.snapshot())
        return points

    def _merged_tags(self, tags: Optional[Dict[str, str]]) -> Dict[str, str]:
        merged = dict(self._global_tags)
        if tags:
            merged.update(tags)
        return merged

    @staticmethod
    def _key(name: str, tags: Optional[Dict[str, str]]) -> str:
        if not tags:
            return name
        tag_str = ",".join(f"{k}={v}" for k, v in sorted(tags.items()))
        return f"{name}{{{tag_str}}}"
