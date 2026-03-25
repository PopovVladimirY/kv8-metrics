"""
Kafka consumer that feeds aggregated metrics into the viewer's MetricStore.
"""

import json
import logging
import threading

from .storage import MetricStore

logger = logging.getLogger(__name__)


class MetricsConsumer(threading.Thread):
    """
    Background thread that subscribes to ``kv8.metrics.aggregated`` and
    records every message into a :class:`~kv8_viewer.storage.MetricStore`.
    """

    def __init__(
        self,
        store: MetricStore,
        bootstrap_servers: str = "localhost:9092",
        topic: str = "kv8.metrics.aggregated",
        consumer_group: str = "kv8-viewer",
    ) -> None:
        super().__init__(name="kv8-viewer-consumer", daemon=True)
        self._store = store
        self._topic = topic
        self._stop_event = threading.Event()
        self._consumer = None

        try:
            from confluent_kafka import Consumer  # type: ignore

            self._consumer = Consumer(
                {
                    "bootstrap.servers": bootstrap_servers,
                    "group.id": consumer_group,
                    "auto.offset.reset": "earliest",
                    "enable.auto.commit": True,
                }
            )
            logger.info("MetricsConsumer connected to Kafka at %s", bootstrap_servers)
        except ImportError:
            logger.warning("confluent-kafka not installed; viewer will show no data from Kafka.")
        except Exception as exc:
            logger.warning("Kafka connection failed: %s", exc)

    def run(self) -> None:
        if self._consumer is None:
            return

        self._consumer.subscribe([self._topic])
        logger.info("Viewer subscribed to %s", self._topic)
        try:
            while not self._stop_event.is_set():
                msg = self._consumer.poll(timeout=1.0)
                if msg is None or msg.error():
                    continue
                try:
                    metric = json.loads(msg.value().decode("utf-8"))
                    self._store.record(metric)
                except Exception:
                    logger.exception("Error storing metric")
        finally:
            self._consumer.close()

    def stop(self) -> None:
        self._stop_event.set()
