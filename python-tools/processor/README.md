# kv8-processor

Aggregation service for the kv8-metrics ecosystem.

Consumes raw metric snapshots from `kv8.metrics.raw`, aggregates them in
fixed-size time windows, and publishes `AggregatedMetric` records to
`kv8.metrics.aggregated`.

## Running

```bash
pip install -r requirements.txt
python main.py \
  --bootstrap-servers kafka:9092 \
  --input-topic  kv8.metrics.raw \
  --output-topic kv8.metrics.aggregated \
  --window 60
```

## Tests

```bash
pytest tests/ -v
```
