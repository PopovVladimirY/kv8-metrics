"""Tests for MetricStore (viewer in-memory storage)."""

import time

import pytest

import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from kv8_viewer.storage import MetricStore


def make_record(name, mean, window_offset=0, tags=None):
    now = time.time() + window_offset
    return {
        "name": name,
        "type": "gauge",
        "mean": mean,
        "value": mean,
        "count": 1,
        "sum": mean,
        "min": mean,
        "max": mean,
        "window_start": now - 60,
        "window_end": now,
        "tags": tags or {},
    }


class TestMetricStore:
    def test_record_and_retrieve_latest(self):
        store = MetricStore()
        store.record(make_record("cpu", 0.5))
        entry = store.latest("cpu")
        assert entry is not None
        assert entry.name == "cpu"
        assert entry.mean == 0.5

    def test_latest_returns_none_for_unknown_metric(self):
        store = MetricStore()
        assert store.latest("nonexistent") is None

    def test_list_metrics_returns_all_names(self):
        store = MetricStore()
        store.record(make_record("cpu", 0.5))
        store.record(make_record("mem", 256))
        names = store.list_metrics()
        assert "cpu" in names
        assert "mem" in names

    def test_list_metrics_empty_when_no_records(self):
        store = MetricStore()
        assert store.list_metrics() == []

    def test_query_returns_records_in_range(self):
        store = MetricStore()
        now = time.time()
        store.record(make_record("lat", 0.1))
        results = store.query("lat", since=now - 120, until=now + 10)
        assert len(results) == 1

    def test_query_excludes_out_of_range(self):
        store = MetricStore()
        now = time.time()
        store.record(make_record("lat", 0.1))
        # Query in the far future — should return nothing
        results = store.query("lat", since=now + 1000, until=now + 2000)
        assert results == []

    def test_multiple_records_ordered_by_insertion(self):
        store = MetricStore()
        for v in [1.0, 2.0, 3.0]:
            store.record(make_record("x", v))
        entry = store.latest("x")
        assert entry.mean == 3.0

    def test_max_points_enforced(self):
        store = MetricStore(max_points_per_metric=3)
        for v in range(10):
            store.record(make_record("y", float(v)))
        # Only last 3 should be kept
        results = store.query("y", since=0, until=time.time() + 100)
        assert len(results) <= 3

    def test_tags_preserved(self):
        store = MetricStore()
        store.record(make_record("req", 10, tags={"service": "api", "env": "prod"}))
        entry = store.latest("req")
        assert entry.tags["service"] == "api"
        assert entry.tags["env"] == "prod"
