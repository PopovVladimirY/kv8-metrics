"""Tests for MetricsCollector."""

import threading
import time
from typing import List
from unittest.mock import MagicMock, patch

import pytest

from kv8_client.collector import MetricsCollector
from kv8_client.metrics import MetricPoint
from kv8_client.producer import KafkaMetricsProducer


class FakeProducer:
    """In-memory producer for testing."""

    def __init__(self):
        self.published: List[List[MetricPoint]] = []
        self.closed = False

    def publish(self, points: List[MetricPoint]) -> None:
        self.published.append(list(points))

    def close(self) -> None:
        self.closed = True


def make_collector(**kwargs) -> tuple:
    fake = FakeProducer()
    collector = MetricsCollector(
        service="test-svc",
        producer=fake,
        **kwargs,
    )
    return collector, fake


class TestMetricsCollectorRegistry:
    def test_counter_returns_same_instance(self):
        collector, _ = make_collector()
        c1 = collector.counter("hits")
        c2 = collector.counter("hits")
        assert c1 is c2

    def test_different_tags_create_different_metrics(self):
        collector, _ = make_collector()
        c1 = collector.counter("hits", tags={"env": "prod"})
        c2 = collector.counter("hits", tags={"env": "staging"})
        assert c1 is not c2

    def test_global_tags_merged_into_metrics(self):
        collector, _ = make_collector(global_tags={"region": "eu-west"})
        c = collector.counter("events")
        assert c.tags["region"] == "eu-west"
        assert c.tags["service"] == "test-svc"

    def test_explicit_tags_override_global(self):
        collector, _ = make_collector(global_tags={"env": "prod"})
        c = collector.counter("req", tags={"env": "staging"})
        assert c.tags["env"] == "staging"

    def test_gauge_registry(self):
        collector, _ = make_collector()
        g1 = collector.gauge("mem")
        g2 = collector.gauge("mem")
        assert g1 is g2

    def test_histogram_registry(self):
        collector, _ = make_collector()
        h1 = collector.histogram("lat")
        h2 = collector.histogram("lat")
        assert h1 is h2

    def test_timer_registry(self):
        collector, _ = make_collector()
        t1 = collector.timer("query")
        t2 = collector.timer("query")
        assert t1 is t2


class TestMetricsCollectorFlush:
    def test_flush_publishes_all_registered_metrics(self):
        collector, fake = make_collector()
        collector.counter("hits").increment(3)
        collector.gauge("mem").set(256)
        collector.histogram("lat").observe(0.1)
        collector.timer("db").start()
        collector.timer("db").stop()

        count = collector.flush()
        assert count == 4
        assert len(fake.published) == 1
        assert len(fake.published[0]) == 4

    def test_flush_returns_zero_when_no_metrics(self):
        collector, fake = make_collector()
        count = collector.flush()
        assert count == 0
        assert fake.published == []

    def test_stop_calls_producer_close(self):
        collector, fake = make_collector()
        collector.stop()
        assert fake.closed is True


class TestMetricsCollectorBackground:
    def test_background_flush_fires_periodically(self):
        collector, fake = make_collector(flush_interval_seconds=0.05)
        collector.counter("ev").increment()
        collector.start()
        time.sleep(0.2)
        collector.stop()
        # Should have flushed at least once
        assert len(fake.published) >= 1

    def test_start_stop_idempotent(self):
        collector, _ = make_collector(flush_interval_seconds=0.1)
        collector.start()
        collector.start()  # second start should be no-op
        collector.stop()
