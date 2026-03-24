#!/usr/bin/env python3
"""Entry-point for the kv8-viewer service."""

import argparse
import uvicorn

from kv8_viewer.app import create_app


def main(argv=None):
    parser = argparse.ArgumentParser(description="kv8-metrics viewer service")
    parser.add_argument("--bootstrap-servers", default="localhost:9092")
    parser.add_argument("--topic", default="kv8.metrics.aggregated")
    parser.add_argument("--group", default="kv8-viewer")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args(argv)

    app = create_app(
        bootstrap_servers=args.bootstrap_servers,
        topic=args.topic,
        consumer_group=args.group,
    )

    uvicorn.run(app, host=args.host, port=args.port)


if __name__ == "__main__":
    main()
