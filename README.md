# kv8-metrics

**kv8** is a fast, low-latency telemetry ecosystem built on top of [Apache Kafka](https://kafka.apache.org/).
It covers the full metrics pipeline — from a lightweight client library that instruments your code, through a
streaming aggregation processor, to a rule-based alerting monitor and a REST viewer API.

```
┌─────────────────────┐     Kafka topic          ┌──────────────────────┐
│   Your Application  │ ──► kv8.metrics.raw  ──► │  kv8-processor       │
│  (kv8-client lib)   │                           │  (aggregation)       │
└─────────────────────┘                           └──────────┬───────────┘
                                                             │ kv8.metrics.aggregated
                              ┌──────────────────────────────┼───────────────────┐
                              ▼                              ▼                   ▼
                   ┌──────────────────┐          ┌───────────────────┐
                   │   kv8-monitor    │          │   kv8-viewer      │
                   │  (alert rules)   │          │  (REST API)       │
                   └──────────┬───────┘          └───────────────────┘
                              │ kv8.metrics.alerts     GET /metrics/{name}/series
                              ▼
                        (log / Kafka)
```

---

## Components

| Component | Description | Location |
|-----------|-------------|----------|
| **kv8-client** | Python library for instrumenting applications | [`client/`](client/) |
| **kv8-processor** | Aggregation service (raw → windowed summaries) | [`processor/`](processor/) |
| **kv8-monitor** | Alert rule engine | [`monitor/`](monitor/) |
| **kv8-viewer** | REST API for querying metrics | [`viewer/`](viewer/) |

---

## Quick Start

### Prerequisites

- Docker & Docker Compose
- Python ≥ 3.9 (for local development)

### Run the full stack

```bash
docker compose up -d
```

This starts:
- Apache Kafka (KRaft mode — no Zookeeper required)
- kv8-processor
- kv8-monitor
- kv8-viewer (exposed on `http://localhost:8080`)

### Verify the viewer is running

```bash
curl http://localhost:8080/health
# {"status":"ok","timestamp":1234567890.123}

curl http://localhost:8080/metrics
# {"metrics":[],"total":0}
```

---

## Client Library

### Installation

```bash
# Core library (no Kafka dependency — useful for testing)
pip install ./client

# With Kafka support
pip install "./client[kafka]"
```

### Instrumenting your code

```python
from kv8_client import MetricsCollector

# Create a collector for your service
collector = MetricsCollector(
    service="payment-service",
    kafka_bootstrap_servers="kafka:9092",
    flush_interval_seconds=10,
    global_tags={"env": "production", "region": "eu-west-1"},
)
collector.start()

# Counter — monotonically increasing event count
requests = collector.counter("http.requests", tags={"endpoint": "/checkout"})
errors   = collector.counter("http.errors",   tags={"endpoint": "/checkout"})

# Gauge — current instantaneous value
active_sessions = collector.gauge("active_sessions")

# Histogram — distribution of observed values
payload_size = collector.histogram("http.request_bytes")

# Timer — measures durations (context manager or manual start/stop)
db_query_time = collector.timer("db.query_seconds")

# --- Application logic ---
active_sessions.increment()
requests.increment()

with db_query_time:
    result = db.execute("SELECT ...")

payload_size.observe(len(request.body))

try:
    process(result)
except Exception:
    errors.increment()
finally:
    active_sessions.decrement()

# On shutdown
collector.stop()
```

### Metric types

| Type | Use case | Key methods |
|------|----------|-------------|
| `Counter` | Monotonically increasing counts | `increment(amount=1)` |
| `Gauge` | Fluctuating point-in-time values | `set(v)`, `increment()`, `decrement()` |
| `Histogram` | Distribution of observed values | `observe(v)` |
| `Timer` | Duration measurements | `start()` / `stop()` or context manager |

---

## Processor

The processor consumes raw `MetricPoint` snapshots from the `kv8.metrics.raw` topic and aggregates them
into fixed-length time windows (default: 60 s).  Each closed window produces an `AggregatedMetric`
published to `kv8.metrics.aggregated`.

```bash
cd processor
pip install -r requirements.txt
python main.py --bootstrap-servers kafka:9092 --window 60
```

**Aggregated fields per window:** `count`, `sum`, `min`, `max`, `mean`.

---

## Monitor

The monitor consumes `AggregatedMetric` records and evaluates configurable alert rules.

```bash
cd monitor
pip install -r requirements.txt
python main.py --bootstrap-servers kafka:9092
```

Built-in default rules:

| Rule name | Metric | Condition | Severity |
|-----------|--------|-----------|----------|
| `high-error-rate` | `http.errors` | mean > 10 per window | CRITICAL |
| `high-latency` | `http.latency_seconds` | mean > 1 s | WARNING |

Custom rules can be added programmatically:

```python
from kv8_monitor import AlertRule, MetricsMonitor, Severity

monitor = MetricsMonitor(bootstrap_servers="kafka:9092")
monitor.add_rule(AlertRule(
    name="low-disk",
    metric_name="disk.free_bytes",
    condition=lambda v: v < 1_000_000_000,   # < 1 GB
    severity=Severity.CRITICAL,
    cooldown_seconds=300,
))
monitor.run()
```

---

## Viewer

The viewer exposes a read-only REST API over aggregated metrics stored in memory.

```bash
cd viewer
pip install -r requirements.txt
python main.py --host 0.0.0.0 --port 8080
```

### Endpoints

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/health` | Health check |
| `GET` | `/metrics` | List all known metric names |
| `GET` | `/metrics/{name}/latest` | Most recent snapshot |
| `GET` | `/metrics/{name}/series?since=&until=` | Time-series query |

#### Example

```bash
# List all metrics
curl http://localhost:8080/metrics

# Get latest value
curl http://localhost:8080/metrics/http.requests/latest

# Query a time range (Unix timestamps)
curl "http://localhost:8080/metrics/http.latency_seconds/series?since=1700000000&until=1700003600"
```

Interactive API docs are available at `http://localhost:8080/docs`.

---

## Kafka Topics

| Topic | Producers | Consumers | Content |
|-------|-----------|-----------|---------|
| `kv8.metrics.raw` | kv8-client | kv8-processor | Raw `MetricPoint` snapshots |
| `kv8.metrics.aggregated` | kv8-processor | kv8-monitor, kv8-viewer | `AggregatedMetric` windows |
| `kv8.metrics.alerts` | kv8-monitor | (custom) | Fired `Alert` records |

### Message format — raw metric

```json
{
  "name": "http.requests",
  "value": 1.0,
  "type": "counter",
  "timestamp": 1700000000.123,
  "tags": {"service": "api", "endpoint": "/users", "env": "production"}
}
```

### Message format — aggregated metric

```json
{
  "name": "http.requests",
  "type": "counter",
  "window_start": 1700000000.0,
  "window_end":   1700000060.0,
  "count": 142,
  "sum": 142.0,
  "min": 1.0,
  "max": 1.0,
  "mean": 1.0,
  "tags": {"service": "api", "env": "production"}
}
```

---

## Running Tests

```bash
# Client library
cd client && pip install -e ".[dev]" && python -m pytest tests/ -v

# Processor
cd processor && python -m pytest tests/ -v

# Monitor
cd monitor && python -m pytest tests/ -v

# Viewer
cd viewer && python -m pytest tests/ -v
```

---

## License

MIT — see [LICENSE](LICENSE).
