# kv8-viewer

REST API viewer for the kv8-metrics ecosystem.

Subscribes to `kv8.metrics.aggregated` and exposes the data through a
[FastAPI](https://fastapi.tiangolo.com/) REST interface.

## Running

```bash
pip install -r requirements.txt
python main.py --bootstrap-servers kafka:9092 --port 8080
```

## Endpoints

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/health` | Health check |
| `GET` | `/metrics` | List all metric names |
| `GET` | `/metrics/{name}/latest` | Most recent snapshot |
| `GET` | `/metrics/{name}/series` | Time-series query |

Interactive docs: `http://localhost:8080/docs`

## Tests

```bash
pip install fastapi pydantic pytest
pytest tests/ -v
```
