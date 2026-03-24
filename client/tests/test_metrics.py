"""Tests for kv8_client metric types."""

import math
import time

import pytest

from kv8_client.metrics import Counter, Gauge, Histogram, MetricPoint, Timer


class TestCounter:
    def test_initial_value_is_zero(self):
        c = Counter("test.counter")
        assert c.value == 0.0

    def test_increment_default(self):
        c = Counter("test.counter")
        c.increment()
        assert c.value == 1.0

    def test_increment_custom_amount(self):
        c = Counter("test.counter")
        c.increment(5)
        c.increment(3.5)
        assert c.value == 8.5

    def test_negative_increment_raises(self):
        c = Counter("test.counter")
        with pytest.raises(ValueError):
            c.increment(-1)

    def test_snapshot_type(self):
        c = Counter("test.counter")
        c.increment(2)
        snap = c.snapshot()
        assert isinstance(snap, MetricPoint)
        assert snap.metric_type == "counter"
        assert snap.name == "test.counter"
        assert snap.value == 2.0

    def test_snapshot_includes_tags(self):
        c = Counter("req", tags={"service": "api", "env": "prod"})
        snap = c.snapshot()
        assert snap.tags["service"] == "api"
        assert snap.tags["env"] == "prod"

    def test_snapshot_timestamp_is_recent(self):
        before = time.time()
        c = Counter("t")
        snap = c.snapshot()
        after = time.time()
        assert before <= snap.timestamp <= after

    def test_to_dict_contains_expected_keys(self):
        c = Counter("hits", tags={"region": "us-east"})
        c.increment(10)
        d = c.snapshot().to_dict()
        assert d["name"] == "hits"
        assert d["value"] == 10.0
        assert d["type"] == "counter"
        assert "timestamp" in d
        assert d["tags"]["region"] == "us-east"

    def test_thread_safety(self):
        import threading

        c = Counter("concurrent")
        threads = [threading.Thread(target=lambda: c.increment()) for _ in range(100)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        assert c.value == 100.0


class TestGauge:
    def test_initial_value_is_zero(self):
        g = Gauge("mem")
        assert g.value == 0.0

    def test_set(self):
        g = Gauge("mem")
        g.set(512.0)
        assert g.value == 512.0

    def test_increment_decrement(self):
        g = Gauge("connections")
        g.increment(3)
        g.decrement(1)
        assert g.value == 2.0

    def test_snapshot_type(self):
        g = Gauge("cpu")
        g.set(0.75)
        snap = g.snapshot()
        assert snap.metric_type == "gauge"
        assert snap.value == 0.75

    def test_can_go_negative(self):
        g = Gauge("delta")
        g.decrement(5)
        assert g.value == -5.0


class TestHistogram:
    def test_empty_histogram_count_zero(self):
        h = Histogram("latency")
        assert h.count == 0

    def test_observe_increments_count(self):
        h = Histogram("latency")
        h.observe(0.1)
        h.observe(0.5)
        assert h.count == 2

    def test_mean_after_observations(self):
        h = Histogram("values")
        h.observe(2.0)
        h.observe(4.0)
        assert math.isclose(h.mean, 3.0)

    def test_empty_mean_is_zero(self):
        h = Histogram("h")
        assert h.mean == 0.0

    def test_snapshot_type(self):
        h = Histogram("req_size")
        h.observe(100)
        snap = h.snapshot()
        assert snap.metric_type == "histogram"
        assert snap.name == "req_size"

    def test_snapshot_tags_include_distribution_info(self):
        h = Histogram("lat")
        h.observe(0.01)
        h.observe(0.1)
        snap = h.snapshot()
        assert "count" in snap.tags
        assert snap.tags["count"] == "2"
        assert "min" in snap.tags
        assert "max" in snap.tags
        assert "buckets" in snap.tags

    def test_custom_buckets(self):
        h = Histogram("size", buckets=[10, 100, 1000])
        h.observe(5)
        h.observe(50)
        h.observe(500)
        snap = h.snapshot()
        # buckets tag should contain 3 entries
        bucket_entries = snap.tags["buckets"].split(",")
        assert len(bucket_entries) == 3


class TestTimer:
    def test_count_starts_at_zero(self):
        t = Timer("db.query")
        assert t.count == 0

    def test_context_manager_records_duration(self):
        t = Timer("op")
        with t:
            time.sleep(0.01)
        assert t.count == 1

    def test_multiple_timings(self):
        t = Timer("work")
        for _ in range(5):
            with t:
                pass
        assert t.count == 5

    def test_snapshot_type(self):
        t = Timer("query")
        with t:
            pass
        snap = t.snapshot()
        assert snap.metric_type == "timer"

    def test_stop_without_start_raises(self):
        t = Timer("x")
        with pytest.raises(RuntimeError):
            t.stop()

    def test_elapsed_is_non_negative(self):
        t = Timer("y")
        t.start()
        elapsed = t.stop()
        assert elapsed >= 0.0
