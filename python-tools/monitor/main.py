#!/usr/bin/env python3
"""Entry-point for the kv8-monitor service."""

import argparse
import logging
import sys

from kv8_monitor.monitor import AlertRule, MetricsMonitor, Severity

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)-8s %(name)s: %(message)s",
)


def main(argv=None):
    parser = argparse.ArgumentParser(description="kv8-metrics monitor / alerting service")
    parser.add_argument("--bootstrap-servers", default="localhost:9092")
    parser.add_argument("--input-topic", default="kv8.metrics.aggregated")
    parser.add_argument("--alerts-topic", default="kv8.metrics.alerts")
    parser.add_argument("--group", default="kv8-monitor")
    args = parser.parse_args(argv)

    # Default built-in rules (operators can extend via code or config)
    default_rules = [
        AlertRule(
            name="high-error-rate",
            metric_name="http.errors",
            condition=lambda v: v > 10,
            severity=Severity.CRITICAL,
            message_template=(
                "High error rate detected: '{metric}' = {value:.2f} errors/window "
                "(tags={tags})"
            ),
        ),
        AlertRule(
            name="high-latency",
            metric_name="http.latency_seconds",
            condition=lambda v: v > 1.0,
            severity=Severity.WARNING,
            message_template=(
                "Elevated latency: '{metric}' mean = {value:.3f}s (tags={tags})"
            ),
        ),
    ]

    monitor = MetricsMonitor(
        rules=default_rules,
        bootstrap_servers=args.bootstrap_servers,
        input_topic=args.input_topic,
        alerts_topic=args.alerts_topic,
        consumer_group=args.group,
    )

    try:
        monitor.run()
    except KeyboardInterrupt:
        monitor.stop()
        sys.exit(0)


if __name__ == "__main__":
    main()
