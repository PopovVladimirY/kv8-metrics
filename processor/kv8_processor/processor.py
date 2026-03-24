"""
kv8-processor — aggregation service for the kv8-metrics ecosystem.

Consumes raw metric snapshots from the ``kv8.metrics.raw`` Kafka topic,
aggregates them in fixed-size time windows, and publishes summarised
:class:`AggregatedMetric` records to ``kv8.metrics.aggregated``.
"""

import json
import logging
import time
import threading
from collections import defaultdict
from dataclasses import dataclass, field
from typing import Dict, List, Optional

logger = logging.getLogger(__name__)


@dataclass
class AggregatedMetric:
    """Result of aggregating a stream of metric points within a time window."""

    name: str
    metric_type: str
    window_start: float
    window_end: float
    count: int
    sum: float
    min: float
    max: float
    tags: Dict[str, str] = field(default_factory=dict)

    @property
    def mean(self) -> float:
        return self.sum / self.count if self.count > 0 else 0.0

    def to_dict(self) -> Dict:
        return {
            "name": self.name,
            "type": self.metric_type,
            "window_start": self.window_start,
            "window_end": self.window_end,
            "count": self.count,
            "sum": self.sum,
            "min": self.min,
            "max": self.max,
            "mean": self.mean,
            "tags": self.tags,
        }


class WindowAggregator:
    """
    Accumulates metric points within rolling time windows and emits
    :class:`AggregatedMetric` objects when each window closes.

    Parameters
    ----------
    window_seconds:
        Duration of each aggregation window in seconds.
    """

    def __init__(self, window_seconds: float = 60.0) -> None:
        self._window = window_seconds
        self._lock = threading.Lock()
        # key -> (metric_type, tags, count, sum, min, max, window_start)
        self._buckets: Dict[str, list] = {}

    def add(self, point: Dict) -> None:
        """Ingest a raw metric point dict (as decoded from Kafka)."""
        name = point["name"]
        metric_type = point.get("type", "gauge")
        value = float(point.get("value", 0))
        tags = point.get("tags", {})
        ts = float(point.get("timestamp", time.time()))

        window_start = (ts // self._window) * self._window
        key = f"{name}|{metric_type}|{window_start}|{sorted(tags.items())}"

        with self._lock:
            if key not in self._buckets:
                self._buckets[key] = [
                    name, metric_type, tags, 0, 0.0, float("inf"), float("-inf"), window_start
                ]
            bucket = self._buckets[key]
            bucket[3] += 1          # count
            bucket[4] += value      # sum
            if value < bucket[5]:
                bucket[5] = value   # min
            if value > bucket[6]:
                bucket[6] = value   # max

    def flush_closed_windows(self) -> List[AggregatedMetric]:
        """
        Return and remove all completed (past) time windows.

        A window is considered closed when its end time is in the past.
        """
        now = time.time()
        closed_before = (now // self._window) * self._window
        results: List[AggregatedMetric] = []

        with self._lock:
            to_delete = []
            for key, bucket in self._buckets.items():
                name, metric_type, tags, count, total, mn, mx, win_start = bucket
                win_end = win_start + self._window
                if win_end <= closed_before:
                    results.append(
                        AggregatedMetric(
                            name=name,
                            metric_type=metric_type,
                            window_start=win_start,
                            window_end=win_end,
                            count=count,
                            sum=total,
                            min=mn,
                            max=mx,
                            tags=tags,
                        )
                    )
                    to_delete.append(key)
            for key in to_delete:
                del self._buckets[key]

        return results

    def flush_all(self) -> List[AggregatedMetric]:
        """Flush all windows regardless of whether they are closed."""
        now = time.time()
        results: List[AggregatedMetric] = []

        with self._lock:
            for key, bucket in list(self._buckets.items()):
                name, metric_type, tags, count, total, mn, mx, win_start = bucket
                if count == 0:
                    continue
                results.append(
                    AggregatedMetric(
                        name=name,
                        metric_type=metric_type,
                        window_start=win_start,
                        window_end=now,
                        count=count,
                        sum=total,
                        min=mn if mn != float("inf") else 0.0,
                        max=mx if mx != float("-inf") else 0.0,
                        tags=tags,
                    )
                )
            self._buckets.clear()

        return results


class MetricsProcessor:
    """
    Service that consumes raw metrics from Kafka, aggregates them, and
    publishes the aggregated results back to Kafka.

    When ``confluent-kafka`` is not available the processor logs aggregated
    metrics instead of publishing them.

    Parameters
    ----------
    bootstrap_servers:
        Kafka broker address(es).
    input_topic:
        Topic to consume raw metrics from.
    output_topic:
        Topic to publish aggregated metrics to.
    consumer_group:
        Kafka consumer group ID.
    window_seconds:
        Aggregation window size in seconds.
    """

    def __init__(
        self,
        bootstrap_servers: str = "localhost:9092",
        input_topic: str = "kv8.metrics.raw",
        output_topic: str = "kv8.metrics.aggregated",
        consumer_group: str = "kv8-processor",
        window_seconds: float = 60.0,
    ) -> None:
        self._input_topic = input_topic
        self._output_topic = output_topic
        self._aggregator = WindowAggregator(window_seconds=window_seconds)
        self._stop_event = threading.Event()

        self._consumer = None
        self._producer = None

        try:
            from confluent_kafka import Consumer, Producer  # type: ignore

            self._consumer = Consumer(
                {
                    "bootstrap.servers": bootstrap_servers,
                    "group.id": consumer_group,
                    "auto.offset.reset": "latest",
                    "enable.auto.commit": True,
                }
            )
            self._producer = Producer({"bootstrap.servers": bootstrap_servers})
            logger.info(
                "MetricsProcessor connected to Kafka at %s", bootstrap_servers
            )
        except ImportError:
            logger.warning(
                "confluent-kafka not installed; running in log-only mode."
            )
        except Exception as exc:
            logger.warning("Kafka connection failed: %s; log-only mode.", exc)

    def run(self) -> None:
        """
        Block and process messages until :meth:`stop` is called.

        This method is intended to be run in the main thread or as a
        long-running background thread.
        """
        if self._consumer is not None:
            self._consumer.subscribe([self._input_topic])
            logger.info("Subscribed to %s", self._input_topic)

        try:
            while not self._stop_event.is_set():
                self._poll_once()
                self._emit_closed_windows()
        finally:
            if self._consumer is not None:
                self._consumer.close()

    def stop(self) -> None:
        self._stop_event.set()

    def _poll_once(self) -> None:
        if self._consumer is None:
            self._stop_event.wait(1.0)
            return

        msg = self._consumer.poll(timeout=1.0)
        if msg is None or msg.error():
            return

        try:
            point = json.loads(msg.value().decode("utf-8"))
            self._aggregator.add(point)
        except Exception:
            logger.exception("Failed to process message")

    def _emit_closed_windows(self) -> None:
        aggregated = self._aggregator.flush_closed_windows()
        for agg in aggregated:
            payload = json.dumps(agg.to_dict()).encode("utf-8")
            if self._producer is not None:
                self._producer.produce(
                    topic=self._output_topic,
                    value=payload,
                    key=agg.name.encode("utf-8"),
                )
                self._producer.poll(0)
            else:
                logger.info("aggregated metric: %s", agg.to_dict())
