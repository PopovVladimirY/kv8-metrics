"""
kv8-viewer FastAPI application.

Provides REST endpoints for querying aggregated metrics stored in memory by
the :class:`~kv8_viewer.consumer.MetricsConsumer`.
"""

from __future__ import annotations

import time
from contextlib import asynccontextmanager
from typing import List, Optional

from fastapi import FastAPI, HTTPException, Query
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel

from .consumer import MetricsConsumer
from .storage import MetricStore, StoredMetric

# ---------------------------------------------------------------------------
# Shared state (module-level singletons configured at startup)
# ---------------------------------------------------------------------------

_store: MetricStore = MetricStore()
_consumer: Optional[MetricsConsumer] = None


# ---------------------------------------------------------------------------
# Pydantic response models
# ---------------------------------------------------------------------------


class MetricPointResponse(BaseModel):
    name: str
    metric_type: str
    window_start: float
    window_end: float
    count: int
    sum: float
    min: float
    max: float
    mean: float
    tags: dict


class MetricsListResponse(BaseModel):
    metrics: List[str]
    total: int


# ---------------------------------------------------------------------------
# Application factory
# ---------------------------------------------------------------------------


def create_app(
    bootstrap_servers: str = "localhost:9092",
    topic: str = "kv8.metrics.aggregated",
    consumer_group: str = "kv8-viewer",
    max_points_per_metric: int = 1440,
    retention_seconds: float = 86400.0,
) -> FastAPI:
    """
    Create and configure the FastAPI viewer application.

    Parameters
    ----------
    bootstrap_servers:
        Kafka broker(s).
    topic:
        Aggregated metrics topic to subscribe to.
    consumer_group:
        Kafka consumer group.
    max_points_per_metric:
        Ring-buffer size per metric.
    retention_seconds:
        Data retention window (seconds).
    """
    global _store, _consumer

    _store = MetricStore(
        max_points_per_metric=max_points_per_metric,
        retention_seconds=retention_seconds,
    )
    _consumer = MetricsConsumer(
        store=_store,
        bootstrap_servers=bootstrap_servers,
        topic=topic,
        consumer_group=consumer_group,
    )

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        _consumer.start()
        yield
        _consumer.stop()

    app = FastAPI(
        title="kv8-metrics Viewer",
        description=(
            "REST API for querying aggregated metrics from the kv8-metrics "
            "telemetry ecosystem."
        ),
        version="0.1.0",
        lifespan=lifespan,
    )

    app.add_middleware(
        CORSMiddleware,
        allow_origins=["*"],
        allow_methods=["GET"],
        allow_headers=["*"],
    )

    # ------------------------------------------------------------------
    # Routes
    # ------------------------------------------------------------------

    @app.get("/health")
    def health():
        """Health-check endpoint."""
        return {"status": "ok", "timestamp": time.time()}

    @app.get("/metrics", response_model=MetricsListResponse)
    def list_metrics():
        """List all metric names that have been observed."""
        names = _store.list_metrics()
        return MetricsListResponse(metrics=names, total=len(names))

    @app.get("/metrics/{name}/latest", response_model=MetricPointResponse)
    def get_latest(name: str):
        """Return the most recent aggregated snapshot for a metric."""
        entry = _store.latest(name)
        if entry is None:
            raise HTTPException(status_code=404, detail=f"Metric '{name}' not found")
        return _to_response(entry)

    @app.get("/metrics/{name}/series", response_model=List[MetricPointResponse])
    def get_series(
        name: str,
        since: Optional[float] = Query(
            default=None,
            description="Unix timestamp — start of the query range (default: now-1h)",
        ),
        until: Optional[float] = Query(
            default=None,
            description="Unix timestamp — end of the query range (default: now)",
        ),
    ):
        """Return a time-series of aggregated snapshots for a metric."""
        points = _store.query(name, since=since, until=until)
        if not points:
            raise HTTPException(
                status_code=404,
                detail=f"No data for metric '{name}' in the requested time range",
            )
        return [_to_response(p) for p in points]

    return app


def _to_response(entry: StoredMetric) -> MetricPointResponse:
    return MetricPointResponse(
        name=entry.name,
        metric_type=entry.metric_type,
        window_start=entry.window_start,
        window_end=entry.window_end,
        count=entry.count,
        sum=entry.sum,
        min=entry.min,
        max=entry.max,
        mean=entry.mean,
        tags=entry.tags,
    )


# Default app instance for ``uvicorn kv8_viewer.app:app``
app = create_app()
