# kv8-client

Python client library for instrumenting applications in the kv8-metrics telemetry ecosystem.

## Installation

```bash
pip install ".[kafka]"   # with Kafka support
pip install "."          # without Kafka (log-only mode, useful for tests)
```

## Usage

```python
from kv8_client import MetricsCollector

collector = MetricsCollector(
    service="my-service",
    kafka_bootstrap_servers="kafka:9092",
    flush_interval_seconds=10,
)
collector.start()

# Register and use metrics
requests = collector.counter("http.requests")
latency  = collector.timer("http.latency_seconds")
mem      = collector.gauge("process.memory_bytes")

requests.increment()
with latency:
    handle_request()

collector.stop()
```

## Development

```bash
pip install -e ".[dev]"
pytest tests/ -v
```
