"""Tests for WindowAggregator and MetricsProcessor."""

import math
import time

import pytest

import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from kv8_processor.processor import AggregatedMetric, WindowAggregator


def make_point(name, value, metric_type="gauge", ts=None, tags=None):
    return {
        "name": name,
        "value": value,
        "type": metric_type,
        "timestamp": ts or time.time(),
        "tags": tags or {},
    }


class TestWindowAggregator:
    def test_empty_flush_returns_empty(self):
        agg = WindowAggregator(window_seconds=60)
        result = agg.flush_all()
        assert result == []

    def test_single_point_flush_all(self):
        agg = WindowAggregator(window_seconds=60)
        agg.add(make_point("cpu", 0.5))
        result = agg.flush_all()
        assert len(result) == 1
        assert result[0].name == "cpu"
        assert math.isclose(result[0].mean, 0.5)

    def test_multiple_points_same_window_aggregated(self):
        agg = WindowAggregator(window_seconds=60)
        now = time.time()
        # Force same window
        window_start = (now // 60) * 60
        for v in [1.0, 2.0, 3.0]:
            agg.add(make_point("lat", v, ts=window_start + 1))
        result = agg.flush_all()
        assert len(result) == 1
        r = result[0]
        assert r.count == 3
        assert math.isclose(r.sum, 6.0)
        assert math.isclose(r.mean, 2.0)
        assert math.isclose(r.min, 1.0)
        assert math.isclose(r.max, 3.0)

    def test_different_metrics_in_separate_buckets(self):
        agg = WindowAggregator(window_seconds=60)
        now = time.time()
        ws = (now // 60) * 60
        agg.add(make_point("cpu", 0.5, ts=ws + 1))
        agg.add(make_point("mem", 256, ts=ws + 1))
        result = agg.flush_all()
        names = {r.name for r in result}
        assert "cpu" in names
        assert "mem" in names
        assert len(result) == 2

    def test_flush_all_clears_state(self):
        agg = WindowAggregator(window_seconds=60)
        agg.add(make_point("x", 1))
        agg.flush_all()
        assert agg.flush_all() == []

    def test_closed_window_detected(self):
        agg = WindowAggregator(window_seconds=60)
        # Add a point in a past window (2 minutes ago)
        old_ts = time.time() - 120
        agg.add(make_point("old_metric", 99, ts=old_ts))
        result = agg.flush_closed_windows()
        assert len(result) == 1
        assert result[0].name == "old_metric"

    def test_current_window_not_closed(self):
        agg = WindowAggregator(window_seconds=60)
        agg.add(make_point("live", 1, ts=time.time()))
        # Current window should NOT be returned as closed
        result = agg.flush_closed_windows()
        assert result == []

    def test_aggregated_metric_to_dict(self):
        m = AggregatedMetric(
            name="test",
            metric_type="counter",
            window_start=1000.0,
            window_end=1060.0,
            count=5,
            sum=50.0,
            min=5.0,
            max=15.0,
        )
        d = m.to_dict()
        assert d["name"] == "test"
        assert d["count"] == 5
        assert math.isclose(d["mean"], 10.0)
        assert d["type"] == "counter"
