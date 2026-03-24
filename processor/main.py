#!/usr/bin/env python3
"""Entry-point for the kv8-processor service."""

import argparse
import logging
import sys

from kv8_processor.processor import MetricsProcessor

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)-8s %(name)s: %(message)s",
)


def main(argv=None):
    parser = argparse.ArgumentParser(description="kv8-metrics processor service")
    parser.add_argument(
        "--bootstrap-servers", default="localhost:9092", help="Kafka broker(s)"
    )
    parser.add_argument(
        "--input-topic", default="kv8.metrics.raw", help="Raw metrics topic"
    )
    parser.add_argument(
        "--output-topic", default="kv8.metrics.aggregated", help="Aggregated metrics topic"
    )
    parser.add_argument(
        "--group", default="kv8-processor", help="Kafka consumer group"
    )
    parser.add_argument(
        "--window", type=float, default=60.0, help="Aggregation window in seconds"
    )
    args = parser.parse_args(argv)

    processor = MetricsProcessor(
        bootstrap_servers=args.bootstrap_servers,
        input_topic=args.input_topic,
        output_topic=args.output_topic,
        consumer_group=args.group,
        window_seconds=args.window,
    )

    try:
        processor.run()
    except KeyboardInterrupt:
        processor.stop()
        sys.exit(0)


if __name__ == "__main__":
    main()
