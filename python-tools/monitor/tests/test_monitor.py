"""Tests for AlertRule, AlertRuleEngine, and MetricsMonitor."""

import time

import pytest

import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from kv8_monitor.monitor import Alert, AlertRule, AlertRuleEngine, MetricsMonitor, Severity


def make_aggregated(name, mean, metric_type="gauge", tags=None):
    now = time.time()
    return {
        "name": name,
        "type": metric_type,
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


class TestAlertRule:
    def test_fires_when_condition_met(self):
        rule = AlertRule(
            name="high-cpu",
            metric_name="cpu.usage",
            condition=lambda v: v > 0.9,
            severity=Severity.WARNING,
            cooldown_seconds=0,
        )
        alert = rule.evaluate("cpu.usage", 0.95, {})
        assert alert is not None
        assert alert.rule_name == "high-cpu"
        assert alert.severity == Severity.WARNING

    def test_does_not_fire_when_condition_not_met(self):
        rule = AlertRule(
            name="high-cpu",
            metric_name="cpu.usage",
            condition=lambda v: v > 0.9,
            cooldown_seconds=0,
        )
        alert = rule.evaluate("cpu.usage", 0.5, {})
        assert alert is None

    def test_does_not_fire_for_different_metric(self):
        rule = AlertRule(
            name="high-cpu",
            metric_name="cpu.usage",
            condition=lambda v: v > 0.9,
            cooldown_seconds=0,
        )
        alert = rule.evaluate("memory.usage", 0.95, {})
        assert alert is None

    def test_cooldown_prevents_repeated_firing(self):
        rule = AlertRule(
            name="test",
            metric_name="x",
            condition=lambda v: True,
            cooldown_seconds=60,
        )
        a1 = rule.evaluate("x", 1.0, {})
        a2 = rule.evaluate("x", 1.0, {})
        assert a1 is not None
        assert a2 is None  # cooldown active

    def test_alert_to_dict(self):
        alert = Alert(
            rule_name="test-rule",
            metric_name="cpu",
            value=0.95,
            severity=Severity.CRITICAL,
            message="Test alert",
            tags={"host": "server1"},
        )
        d = alert.to_dict()
        assert d["rule"] == "test-rule"
        assert d["severity"] == "critical"
        assert d["tags"]["host"] == "server1"


class TestAlertRuleEngine:
    def test_evaluates_all_rules(self):
        engine = AlertRuleEngine(
            rules=[
                AlertRule("r1", "cpu", lambda v: v > 0.9, cooldown_seconds=0),
                AlertRule("r2", "mem", lambda v: v > 0.8, cooldown_seconds=0),
            ]
        )
        metric = make_aggregated("cpu", 0.95)
        alerts = engine.evaluate(metric)
        assert len(alerts) == 1
        assert alerts[0].rule_name == "r1"

    def test_multiple_rules_fire_for_same_metric(self):
        engine = AlertRuleEngine(
            rules=[
                AlertRule("warn", "cpu", lambda v: v > 0.7, severity=Severity.WARNING, cooldown_seconds=0),
                AlertRule("crit", "cpu", lambda v: v > 0.9, severity=Severity.CRITICAL, cooldown_seconds=0),
            ]
        )
        metric = make_aggregated("cpu", 0.95)
        alerts = engine.evaluate(metric)
        assert len(alerts) == 2

    def test_no_alerts_when_no_rules_match(self):
        engine = AlertRuleEngine(
            rules=[AlertRule("r1", "cpu", lambda v: v > 0.9, cooldown_seconds=0)]
        )
        metric = make_aggregated("mem", 0.5)
        alerts = engine.evaluate(metric)
        assert alerts == []

    def test_add_rule_at_runtime(self):
        engine = AlertRuleEngine()
        engine.add_rule(AlertRule("r1", "cpu", lambda v: v > 0.5, cooldown_seconds=0))
        alerts = engine.evaluate(make_aggregated("cpu", 0.8))
        assert len(alerts) == 1
