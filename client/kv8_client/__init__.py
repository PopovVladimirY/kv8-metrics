"""
kv8-client — telemetry client library for the kv8-metrics ecosystem.

Public API::

    from kv8_client import MetricsCollector

    collector = MetricsCollector(
        service="my-service",
        kafka_bootstrap_servers="kafka:9092",
    )
    collector.start()

    requests = collector.counter("http.requests")
    requests.increment()

    collector.stop()
"""

from .collector import MetricsCollector
from .metrics import Counter, Gauge, Histogram, MetricPoint, Timer
from .producer import KafkaMetricsProducer

__all__ = [
    "MetricsCollector",
    "Counter",
    "Gauge",
    "Histogram",
    "Timer",
    "MetricPoint",
    "KafkaMetricsProducer",
]
