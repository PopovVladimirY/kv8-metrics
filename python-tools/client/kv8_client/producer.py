"""
Kafka producer wrapper for publishing metric snapshots.

The :class:`KafkaMetricsProducer` serialises :class:`~kv8_client.metrics.MetricPoint`
objects to JSON and delivers them to a Kafka topic.  When the ``confluent-kafka``
package is not installed the producer silently logs every message — useful for
local development without a running Kafka cluster.
"""

import json
import logging
from typing import List, Optional

from .metrics import MetricPoint

logger = logging.getLogger(__name__)


class KafkaMetricsProducer:
    """
    Thin wrapper around a Kafka producer for delivering metric snapshots.

    Parameters
    ----------
    bootstrap_servers:
        Comma-separated ``host:port`` pairs for the Kafka cluster.
    topic:
        Target Kafka topic name.
    extra_config:
        Additional ``confluent-kafka`` producer configuration keys.
    """

    def __init__(
        self,
        bootstrap_servers: str = "localhost:9092",
        topic: str = "kv8.metrics.raw",
        extra_config: Optional[dict] = None,
    ) -> None:
        self._topic = topic
        self._producer = None
        config = {
            "bootstrap.servers": bootstrap_servers,
            "client.id": "kv8-metrics-client",
            "linger.ms": 20,           # batch small messages for throughput
            "batch.num.messages": 500,
            "compression.type": "lz4",
            **(extra_config or {}),
        }
        try:
            from confluent_kafka import Producer  # type: ignore

            self._producer = Producer(config)
            logger.info(
                "KafkaMetricsProducer connected to %s (topic=%s)",
                bootstrap_servers,
                topic,
            )
        except ImportError:
            logger.warning(
                "confluent-kafka is not installed; metric points will be logged only. "
                "Install it with: pip install confluent-kafka"
            )
        except Exception as exc:
            logger.warning(
                "Could not connect to Kafka at %s: %s. "
                "Metrics will be logged only.",
                bootstrap_servers,
                exc,
            )

    def publish(self, points: List[MetricPoint]) -> None:
        """Serialize *points* to JSON and produce them to the configured topic."""
        for point in points:
            payload = json.dumps(point.to_dict()).encode("utf-8")
            if self._producer is not None:
                self._producer.produce(
                    topic=self._topic,
                    value=payload,
                    key=point.name.encode("utf-8"),
                    on_delivery=self._delivery_report,
                )
            else:
                logger.debug("metric (no-kafka): %s", payload.decode())
        if self._producer is not None:
            self._producer.poll(0)

    def close(self) -> None:
        """Flush pending messages and close the producer."""
        if self._producer is not None:
            remaining = self._producer.flush(timeout=10)
            if remaining:
                logger.warning(
                    "%d metric messages were not delivered before shutdown",
                    remaining,
                )

    @staticmethod
    def _delivery_report(err, msg) -> None:
        if err:
            logger.warning(
                "Metric delivery failed for %s: %s", msg.key(), err
            )
