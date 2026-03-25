"""
kv8-monitor — alerting service for the kv8-metrics ecosystem.

Consumes aggregated metrics from the ``kv8.metrics.aggregated`` Kafka topic
and evaluates configurable alert rules.  When a rule fires, a notification is
emitted to the ``kv8.metrics.alerts`` topic and logged.
"""

import json
import logging
import threading
import time
from dataclasses import dataclass, field
from enum import Enum
from typing import Callable, Dict, List, Optional

logger = logging.getLogger(__name__)


class Severity(str, Enum):
    INFO = "info"
    WARNING = "warning"
    CRITICAL = "critical"


@dataclass
class AlertRule:
    """
    A single alert rule evaluated against incoming aggregated metrics.

    Parameters
    ----------
    name:
        Human-readable rule identifier, e.g. ``"high-error-rate"``.
    metric_name:
        The metric name to match against, e.g. ``"http.errors"``.
    condition:
        A callable ``(value: float) -> bool`` that returns ``True`` when the
        alert should fire.  Example: ``lambda v: v > 0.05``.
    severity:
        Alert severity level.
    message_template:
        f-string template for the alert message.  Available variables are
        ``{name}``, ``{value}``, ``{metric}``, and ``{tags}``.
    cooldown_seconds:
        Minimum seconds between consecutive firings of the same rule.
    """

    name: str
    metric_name: str
    condition: Callable[[float], bool]
    severity: Severity = Severity.WARNING
    message_template: str = "Alert '{name}': metric '{metric}' value {value:.4f} (tags={tags})"
    cooldown_seconds: float = 60.0
    _last_fired: float = field(default=0.0, init=False, repr=False)

    def evaluate(self, metric_name: str, value: float, tags: Dict) -> Optional["Alert"]:
        """Check the rule against the given metric and return an Alert if it fires."""
        if metric_name != self.metric_name:
            return None
        if not self.condition(value):
            return None
        now = time.time()
        if now - self._last_fired < self.cooldown_seconds:
            return None
        self._last_fired = now
        message = self.message_template.format(
            name=self.name, value=value, metric=metric_name, tags=tags
        )
        return Alert(
            rule_name=self.name,
            metric_name=metric_name,
            value=value,
            severity=self.severity,
            message=message,
            tags=tags,
            timestamp=now,
        )


@dataclass
class Alert:
    """A fired alert notification."""

    rule_name: str
    metric_name: str
    value: float
    severity: Severity
    message: str
    tags: Dict
    timestamp: float = field(default_factory=time.time)

    def to_dict(self) -> Dict:
        return {
            "rule": self.rule_name,
            "metric": self.metric_name,
            "value": self.value,
            "severity": self.severity.value,
            "message": self.message,
            "tags": self.tags,
            "timestamp": self.timestamp,
        }


class AlertRuleEngine:
    """Evaluates a set of :class:`AlertRule` objects against metric data."""

    def __init__(self, rules: Optional[List[AlertRule]] = None) -> None:
        self._rules: List[AlertRule] = list(rules or [])
        self._lock = threading.Lock()

    def add_rule(self, rule: AlertRule) -> None:
        with self._lock:
            self._rules.append(rule)

    def evaluate(self, metric: Dict) -> List[Alert]:
        """Evaluate all rules against *metric* and return any fired alerts."""
        name = metric.get("name", "")
        value = float(metric.get("mean", metric.get("value", 0)))
        tags = metric.get("tags", {})
        alerts: List[Alert] = []
        with self._lock:
            for rule in self._rules:
                alert = rule.evaluate(name, value, tags)
                if alert:
                    alerts.append(alert)
        return alerts


class MetricsMonitor:
    """
    Service that watches aggregated metrics and fires alerts.

    Parameters
    ----------
    rules:
        Initial list of alert rules.
    bootstrap_servers:
        Kafka broker address(es).
    input_topic:
        Topic to consume aggregated metrics from.
    alerts_topic:
        Topic to publish fired alerts to.
    consumer_group:
        Kafka consumer group ID.
    """

    def __init__(
        self,
        rules: Optional[List[AlertRule]] = None,
        bootstrap_servers: str = "localhost:9092",
        input_topic: str = "kv8.metrics.aggregated",
        alerts_topic: str = "kv8.metrics.alerts",
        consumer_group: str = "kv8-monitor",
    ) -> None:
        self._input_topic = input_topic
        self._alerts_topic = alerts_topic
        self._engine = AlertRuleEngine(rules)
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
            logger.info("MetricsMonitor connected to Kafka at %s", bootstrap_servers)
        except ImportError:
            logger.warning("confluent-kafka not installed; running in log-only mode.")
        except Exception as exc:
            logger.warning("Kafka connection failed: %s; log-only mode.", exc)

    def add_rule(self, rule: AlertRule) -> None:
        """Add an alert rule at runtime."""
        self._engine.add_rule(rule)

    def run(self) -> None:
        """Block and process messages until :meth:`stop` is called."""
        if self._consumer is not None:
            self._consumer.subscribe([self._input_topic])
            logger.info("Monitor subscribed to %s", self._input_topic)

        try:
            while not self._stop_event.is_set():
                self._poll_once()
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
            metric = json.loads(msg.value().decode("utf-8"))
            alerts = self._engine.evaluate(metric)
            for alert in alerts:
                self._emit_alert(alert)
        except Exception:
            logger.exception("Failed to process aggregated metric")

    def _emit_alert(self, alert: Alert) -> None:
        payload = json.dumps(alert.to_dict()).encode("utf-8")
        level = logging.CRITICAL if alert.severity == Severity.CRITICAL else logging.WARNING
        logger.log(level, "ALERT [%s]: %s", alert.severity.value.upper(), alert.message)
        if self._producer is not None:
            self._producer.produce(
                topic=self._alerts_topic,
                value=payload,
                key=alert.rule_name.encode("utf-8"),
            )
            self._producer.poll(0)
