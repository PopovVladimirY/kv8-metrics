# kv8-monitor

Rules-based alerting service for the kv8-metrics ecosystem.

Consumes aggregated metrics from `kv8.metrics.aggregated`, evaluates configured
alert rules, and publishes fired alerts to `kv8.metrics.alerts`.

## Running

```bash
pip install -r requirements.txt
python main.py --bootstrap-servers kafka:9092
```

## Adding custom rules

```python
from kv8_monitor import AlertRule, MetricsMonitor, Severity

monitor = MetricsMonitor(bootstrap_servers="kafka:9092")
monitor.add_rule(AlertRule(
    name="high-error-rate",
    metric_name="http.errors",
    condition=lambda v: v > 10,
    severity=Severity.CRITICAL,
    cooldown_seconds=60,
))
monitor.run()
```

## Tests

```bash
pytest tests/ -v
```
