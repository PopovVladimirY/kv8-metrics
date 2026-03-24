////////////////////////////////////////////////////////////////////////////////
// kv8/IKv8Producer.h -- abstract producer interface for writing messages to
//                        Kafka topics used by the kv8 telemetry protocol.
//
// Instances are created exclusively through the static factory function
// IKv8Producer::Create().  No librdkafka headers are exposed.
//
// Typical usage
// ─────────────
// kv8probe -- synthetic test data producer:
//
//     auto p = IKv8Producer::Create(cfg);
//     p->Produce(dataTopic, &message, sizeof(message), &keyBE, sizeof(keyBE));
//     p->Flush(30000);
//
// kv8bridge -- control-topic publisher (Baical -> Kafka direction):
//
//     auto p = IKv8Producer::Create(cfg);
//     // ...inside the EnableCallback:
//     p->Produce(controlTopic, "DISABLE 3", 9, &hash, 4);
//     p->Flush(0);   // non-blocking poll (fire-and-forget)
//
// Dual-use scenario (consume + produce in one object):
//
//     The IKv8Client interface inherits both IKv8Consumer and IKv8Producer.
//     See IKv8Client.h for the combined factory.
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Kv8Types.h"

#include <cstdint>
#include <memory>

namespace kv8 {

/// Abstract producer for publishing raw messages to Kafka topics.
///
/// Thread safety: Produce() and Flush() must be called from a single thread.
class IKv8Producer
{
public:
    virtual ~IKv8Producer() = default;

    /// Enqueue one message for delivery to @p sTopic.
    ///
    /// The payload and key bytes are copied internally; the caller's buffers
    /// may be freed as soon as this function returns.
    ///
    /// @param sTopic     Kafka topic name.
    /// @param pPayload   Message value bytes (must not be nullptr if cbPayload > 0).
    /// @param cbPayload  Byte length of the value.
    /// @param pKey       Optional message key (may be nullptr).
    /// @param cbKey      Byte length of the key; 0 when pKey is nullptr.
    /// @return           true on success; false if the internal queue is full.
    virtual bool Produce(const std::string &sTopic,
                         const void        *pPayload,
                         size_t             cbPayload,
                         const void        *pKey  = nullptr,
                         size_t             cbKey  = 0) = 0;

    /// Block until all enqueued messages have been delivered (or failed)
    /// or until @p timeoutMs milliseconds have elapsed.
    ///
    /// Pass 0 to pump the delivery-report callbacks without blocking
    /// (fire-and-forget mode used in real-time control publishing).
    virtual void Flush(int timeoutMs = 10000) = 0;

    /// Return the cumulative count of delivery-report failures (messages
    /// that librdkafka could not deliver to the broker after all retries).
    virtual int64_t GetDeliveryFailures() const { return 0; }

    /// Return the cumulative count of successfully delivered messages.
    virtual int64_t GetDeliverySuccess() const { return 0; }

    // ── Pre-created topic handle API (hot-path optimisation) ──────────────────

    /// Pre-create and cache an opaque topic handle for a given topic name.
    /// Avoids the per-message string hash lookup inside rd_kafka_producev().
    /// Returns nullptr if the operation fails or if not overridden.
    /// The handle MUST be released by calling DestroyTopic() before this
    /// producer is destroyed (before or during kv8log_close).
    virtual void* CreateTopic(const std::string& /*sTopic*/) { return nullptr; }

    /// Release a handle obtained from CreateTopic().
    virtual void DestroyTopic(void* /*hTopic*/) {}

    /// Enqueue a message to a pre-created topic handle from CreateTopic().
    /// This path bypasses the per-message topic string lookup.
    /// Returns false (no-op) if hTopic is nullptr.
    virtual bool ProduceToTopic(void*       hTopic,
                                 const void* pPayload,
                                 size_t      cbPayload,
                                 const void* pKey  = nullptr,
                                 size_t      cbKey = 0)
    {
        (void)hTopic; (void)pPayload; (void)cbPayload; (void)pKey; (void)cbKey;
        return false;
    }

    // ── Heartbeat ─────────────────────────────────────────────────────────────
    //
    // The preferred way to enable heartbeating is to set Kv8Config::sHeartbeatTopic
    // before calling IKv8Producer::Create().  The producer then starts the
    // background thread automatically on connect and sends a clean-shutdown
    // marker automatically when it is destroyed.  No manual calls are required.
    //
    // StartHeartbeat() / StopHeartbeat() remain available for cases where the
    // heartbeat topic is not known until after the producer is created.

    /// Start a background thread that writes one HbRecord to @p sHbTopic
    /// every @p intervalMs milliseconds.
    ///
    /// Calling StartHeartbeat() while one is already running is a no-op.
    ///
    /// @param sHbTopic   Kafka topic name; typically "<sessionPrefix>.hb".
    /// @param intervalMs Milliseconds between successive heartbeat records.
    virtual void StartHeartbeat(const std::string &sHbTopic,
                                int                intervalMs) {}

    /// Stop the heartbeat background thread.
    ///
    /// If @p bSendShutdown is true, a final HbRecord with
    /// bState = KV8_HB_STATE_SHUTDOWN is flushed before the thread exits.
    ///
    /// Blocks until the thread has exited.  Idempotent: safe to call even if
    /// the heartbeat was never started.  Called automatically by the destructor.
    virtual void StopHeartbeat(bool bSendShutdown = true) { (void)bSendShutdown; }

    // ── Factory ────────────────────────────────────────────────────────────────

    /// Create and return a new producer.
    /// Returns nullptr only on a fatal Kafka configuration error.
    static std::unique_ptr<IKv8Producer> Create(const Kv8Config &cfg);
};

} // namespace kv8
