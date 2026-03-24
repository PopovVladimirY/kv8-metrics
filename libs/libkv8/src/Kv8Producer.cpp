////////////////////////////////////////////////////////////////////////////////
// Kv8Producer.cpp -- IKv8Producer implementation
//
// All librdkafka usage is confined to this translation unit.
////////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <Windows.h>
#endif

#include <librdkafka/rdkafka.h>

#include <kv8/IKv8Producer.h>

#include <cstdio>
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>

namespace kv8 {

////////////////////////////////////////////////////////////////////////////////
// Internal helper (duplicated from Kv8Consumer.cpp -- both TUs are separate)
////////////////////////////////////////////////////////////////////////////////

static bool ApplySecurityConfP(rd_kafka_conf_t    *pConf,
                               const Kv8Config    &cfg,
                               char               *errstr,
                               size_t              errlen)
{
    auto set = [&](const char *k, const char *v) -> bool {
        if (rd_kafka_conf_set(pConf, k, v, errstr, errlen) != RD_KAFKA_CONF_OK) {
            fprintf(stderr, "[KV8P] conf_set(%s): %s\n", k, errstr);
            return false;
        }
        return true;
    };
    if (!set("bootstrap.servers",  cfg.sBrokers.c_str()))       return false;
    if (!set("security.protocol",  cfg.sSecurityProto.c_str())) return false;
    if (cfg.sSecurityProto.find("sasl") != std::string::npos)
    {
        if (!set("sasl.mechanism", cfg.sSaslMechanism.c_str())) return false;
        if (!set("sasl.username",  cfg.sUser.c_str()))          return false;
        if (!set("sasl.password",  cfg.sPass.c_str()))          return false;
    }
    return true;
}

////////////////////////////////////////////////////////////////////////////////
// Delivery report callback -- counts failed deliveries per instance.
// The opaque pointer is set to the owning Kv8ProducerImpl* in the
// constructor so each producer tracks its own counters.
// Declared here, defined after the class (needs full class definition).
////////////////////////////////////////////////////////////////////////////////

static void DeliveryReportCb(rd_kafka_t        * /*rk*/,
                              const rd_kafka_message_t *pMsg,
                              void              *opaque);

////////////////////////////////////////////////////////////////////////////////
// Kv8ProducerImpl
////////////////////////////////////////////////////////////////////////////////

class Kv8ProducerImpl final : public IKv8Producer
{
public:
    explicit Kv8ProducerImpl(const Kv8Config &cfg);
    ~Kv8ProducerImpl() override;

    bool Produce(const std::string &sTopic,
                 const void        *pPayload, size_t cbPayload,
                 const void        *pKey  = nullptr,
                 size_t             cbKey  = 0) override;

    void Flush(int timeoutMs = 10000) override;
    int64_t GetDeliveryFailures() const override;
    int64_t GetDeliverySuccess()  const override;

    void* CreateTopic(const std::string& sTopic) override;
    void  DestroyTopic(void* hTopic) override;
    bool  ProduceToTopic(void* hTopic,
                         const void* pPayload, size_t cbPayload,
                         const void* pKey = nullptr, size_t cbKey = 0) override;

    void StartHeartbeat(const std::string &sHbTopic, int intervalMs) override;
    void StopHeartbeat(bool bSendShutdown = true) override;

private:
    // Poll the rdkafka event loop (DR callbacks, retransmit timers) every
    // kPollInterval messages instead of on every single Produce() call.
    // This eliminates the dominant per-message mutex acquisition inside
    // rd_kafka_poll() while still draining the DR queue frequently enough
    // to prevent the internal queue from filling under sustained load.
    static constexpr uint32_t kPollInterval = 512u;
    std::atomic<uint32_t> m_produce_count{0};

    rd_kafka_t *m_rk = nullptr;

    // Per-instance delivery counters (CR-01: were incorrectly file-scope globals).
    std::atomic<int64_t> m_drFailures{0};
    std::atomic<int64_t> m_drSuccess{0};

    // Per-instance error-print throttle (CR-07: was function-scope static).
    std::atomic<int64_t> m_errPrinted{0};

    // Heartbeat background thread state.
    std::thread              m_hbThread;
    std::atomic<bool>        m_hbStop{false};
    std::string              m_sHbTopic;
    int                      m_hbIntervalMs = 3000;

    // Heartbeat config captured from Kv8Config at construction time.
    // Used to auto-start the heartbeat after the Kafka handle is ready.
    std::string              m_sCfgHbTopic;
    int                      m_nCfgHbIntervalMs = 3000;

    // Shared produce logic used by both Produce() and ProduceToTopic().
    void PollIfNeeded() noexcept;
    bool ReportError(rd_kafka_resp_err_t err) noexcept;
    void HeartbeatThreadFunc(std::string sHbTopic, int intervalMs) noexcept;

    friend void DeliveryReportCb(rd_kafka_t*,
                                  const rd_kafka_message_t*,
                                  void*);
};

// Definition placed after the class so we can access m_drFailures / m_drSuccess.
static void DeliveryReportCb(rd_kafka_t        * /*rk*/,
                              const rd_kafka_message_t *pMsg,
                              void              *opaque)
{
    auto *self = static_cast<Kv8ProducerImpl *>(opaque);
    if (!self) return;
    if (pMsg->err != RD_KAFKA_RESP_ERR_NO_ERROR)
    {
        int64_t n = self->m_drFailures.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 5)
            fprintf(stderr, "[KV8P] delivery failed: %s\n",
                    rd_kafka_err2str(pMsg->err));
    }
    else
    {
        self->m_drSuccess.fetch_add(1, std::memory_order_relaxed);
    }
}

Kv8ProducerImpl::Kv8ProducerImpl(const Kv8Config &cfg)
{
    m_sCfgHbTopic      = cfg.sHeartbeatTopic;
    m_nCfgHbIntervalMs = cfg.nHeartbeatIntervalMs > 0 ? cfg.nHeartbeatIntervalMs : 3000;

    char errstr[512];
    rd_kafka_conf_t *pConf = rd_kafka_conf_new();

    if (!ApplySecurityConfP(pConf, cfg, errstr, sizeof(errstr)))
    {
        rd_kafka_conf_destroy(pConf);
        return;
    }

    int nConfErrors = 0;
    auto set = [&](const char *k, const char *v) {
        if (rd_kafka_conf_set(pConf, k, v, errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK)
        {
            fprintf(stderr, "[KV8P] conf_set(%s=%s): %s\n", k, v, errstr);
            ++nConfErrors;
        }
    };

    // Batch settings balanced for low latency + reasonable throughput.
    set("linger.ms",                    "5");
    set("batch.num.messages",           "10000");
    // Disable Nagle's algorithm on the broker socket. Same rationale as the
    // consumer: on Windows the produce path crosses the WSL2/Hyper-V virtual
    // NIC and Nagle+delayed-ACK compounds to ~1 second per ProduceRequest.
    set("socket.nagle.disable",         "true");
    set("queue.buffering.max.messages", "2000000");
    set("queue.buffering.max.kbytes",   "2097152");  // 2 GB

    // Enable idempotent producer: librdkafka will retry failed deliveries
    // transparently (with sequence-number dedup at the broker), eliminating
    // silent drops caused by transient leader changes or timeouts.
    // Requires acks=all (set automatically by librdkafka when idempotence
    // is enabled).  max.in.flight.requests.per.connection is auto-capped
    // to 5 by librdkafka.
    set("enable.idempotence",           "true");

    if (nConfErrors > 0)
    {
        fprintf(stderr, "[KV8P] WARNING: %d config error(s) -- producer may "
                        "not be in idempotent mode!\n", nConfErrors);
    }

    // Install delivery report callback; opaque -> this so the static
    // callback can update this instance's counters (CR-01).
    rd_kafka_conf_set_opaque(pConf, this);
    rd_kafka_conf_set_dr_msg_cb(pConf, DeliveryReportCb);

    m_rk = rd_kafka_new(RD_KAFKA_PRODUCER, pConf, errstr, sizeof(errstr));
    if (!m_rk)
    {
        fprintf(stderr, "[KV8P] rd_kafka_new: %s\n", errstr);
        rd_kafka_conf_destroy(pConf);
    }
    else
    {
        // Confirm idempotent mode is active.
        size_t sz = 0;
        char val[64];
        sz = sizeof(val);
        if (rd_kafka_conf_get(rd_kafka_conf(m_rk),
                "enable.idempotence", val, &sz) == RD_KAFKA_CONF_OK
            && strcmp(val, "true") == 0)
        {
            fprintf(stderr, "[KV8P] Producer: idempotent mode ACTIVE "
                            "(acks=all, retries=auto)\n");
        }
        else
        {
            fprintf(stderr, "[KV8P] WARNING: idempotent mode NOT active "
                            "-- message loss may occur!\n");
        }

        // Auto-start heartbeat if a topic was provided in the config.
        if (!m_sCfgHbTopic.empty())
            StartHeartbeat(m_sCfgHbTopic, m_nCfgHbIntervalMs);
    }
    // pConf owned by m_rk on success
}

Kv8ProducerImpl::~Kv8ProducerImpl()
{
    // Stop heartbeat thread before destroying the Kafka handle.
    StopHeartbeat(true);

    if (m_rk)
    {
        rd_kafka_flush(m_rk, 5000);
        rd_kafka_destroy(m_rk);
        m_rk = nullptr;
    }
}

void Kv8ProducerImpl::PollIfNeeded() noexcept
{
    // Poll every kPollInterval messages. Uses bitwise AND (kPollInterval is a
    // power of two) so the modulo is a single AND instruction with no branch.
    if ((m_produce_count.fetch_add(1, std::memory_order_relaxed)
            & (kPollInterval - 1u)) == 0u)
        rd_kafka_poll(m_rk, 0);
}

bool Kv8ProducerImpl::ReportError(rd_kafka_resp_err_t err) noexcept
{
    // m_errPrinted is per-instance (CR-07: was function-scope static shared
    // across all producers and all error types).
    if (m_errPrinted.fetch_add(1, std::memory_order_relaxed) < 5)
        fprintf(stderr, "[KV8P] producev: %s\n", rd_kafka_err2str(err));
    return false;
}

bool Kv8ProducerImpl::Produce(const std::string &sTopic,
                               const void  *pPayload, size_t cbPayload,
                               const void  *pKey,     size_t cbKey)
{
    if (!m_rk) return false;
    PollIfNeeded();
    rd_kafka_resp_err_t err = rd_kafka_producev(
        m_rk,
        RD_KAFKA_V_TOPIC(sTopic.c_str()),
        RD_KAFKA_V_VALUE(const_cast<void*>(pPayload), cbPayload),
        RD_KAFKA_V_KEY(const_cast<void*>(pKey), cbKey),
        RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
        RD_KAFKA_V_END);
    if (err != RD_KAFKA_RESP_ERR_NO_ERROR)
        return ReportError(err);
    return true;
}

void* Kv8ProducerImpl::CreateTopic(const std::string& sTopic)
{
    if (!m_rk) return nullptr;
    return rd_kafka_topic_new(m_rk, sTopic.c_str(), nullptr);
}

void Kv8ProducerImpl::DestroyTopic(void* hTopic)
{
    if (hTopic)
        rd_kafka_topic_destroy(static_cast<rd_kafka_topic_t*>(hTopic));
}

bool Kv8ProducerImpl::ProduceToTopic(void* hTopic,
                                      const void* pPayload, size_t cbPayload,
                                      const void* pKey,     size_t cbKey)
{
    if (!m_rk || !hTopic) return false;
    PollIfNeeded();
    rd_kafka_resp_err_t err = rd_kafka_producev(
        m_rk,
        RD_KAFKA_V_RKT(static_cast<rd_kafka_topic_t*>(hTopic)),
        RD_KAFKA_V_VALUE(const_cast<void*>(pPayload), cbPayload),
        RD_KAFKA_V_KEY(const_cast<void*>(pKey), cbKey),
        RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
        RD_KAFKA_V_END);
    if (err != RD_KAFKA_RESP_ERR_NO_ERROR)
        return ReportError(err);
    return true;
}

void Kv8ProducerImpl::Flush(int timeoutMs)
{
    if (m_rk)
        rd_kafka_flush(m_rk, timeoutMs);
}

int64_t Kv8ProducerImpl::GetDeliveryFailures() const
{
    return m_drFailures.load(std::memory_order_relaxed);
}

int64_t Kv8ProducerImpl::GetDeliverySuccess() const
{
    return m_drSuccess.load(std::memory_order_relaxed);
}

////////////////////////////////////////////////////////////////////////////////
// Heartbeat
////////////////////////////////////////////////////////////////////////////////

void Kv8ProducerImpl::HeartbeatThreadFunc(std::string sHbTopic,
                                           int intervalMs) noexcept
{
    uint32_t dwSeqNo = 0;
    while (!m_hbStop.load(std::memory_order_acquire))
    {
        // Write one alive heartbeat record.
        kv8::HbRecord hb{};
        hb.bVersion  = KV8_HB_VERSION;
        hb.bState    = KV8_HB_STATE_ALIVE;
        hb.wReserved = 0;
        hb.dwSeqNo   = dwSeqNo++;

        // Wall-clock Unix time in milliseconds.
        using namespace std::chrono;
        hb.tsUnixMs = duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()).count();

        if (m_rk)
        {
            rd_kafka_producev(
                m_rk,
                RD_KAFKA_V_TOPIC(sHbTopic.c_str()),
                RD_KAFKA_V_VALUE(&hb, sizeof(hb)),
                RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
                RD_KAFKA_V_END);
            rd_kafka_poll(m_rk, 0);
        }

        // Sleep in 100 ms slices so we wake up promptly when stopped.
        const int nSlices = (std::max)(1, intervalMs / 100);
        for (int i = 0; i < nSlices; ++i)
        {
            if (m_hbStop.load(std::memory_order_acquire)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void Kv8ProducerImpl::StartHeartbeat(const std::string &sHbTopic, int intervalMs)
{
    if (m_hbThread.joinable()) return; // already running
    m_sHbTopic = sHbTopic;
    m_hbIntervalMs = intervalMs;
    m_hbStop.store(false, std::memory_order_relaxed);
    m_hbThread = std::thread(&Kv8ProducerImpl::HeartbeatThreadFunc,
                              this, sHbTopic, intervalMs);
}

void Kv8ProducerImpl::StopHeartbeat(bool bSendShutdown)
{
    if (!m_hbThread.joinable()) return;

    // Signal the thread to exit.
    m_hbStop.store(true, std::memory_order_release);
    m_hbThread.join();

    // Send a clean-shutdown marker so consumers can distinguish a clean stop
    // from a crash (where no shutdown record would appear).
    if (bSendShutdown && m_rk && !m_sHbTopic.empty())
    {
        kv8::HbRecord hb{};
        hb.bVersion  = KV8_HB_VERSION;
        hb.bState    = KV8_HB_STATE_SHUTDOWN;
        hb.wReserved = 0;
        hb.dwSeqNo   = 0xFFFFFFFFu;

        using namespace std::chrono;
        hb.tsUnixMs = duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()).count();

        rd_kafka_producev(
            m_rk,
            RD_KAFKA_V_TOPIC(m_sHbTopic.c_str()),
            RD_KAFKA_V_VALUE(&hb, sizeof(hb)),
            RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
            RD_KAFKA_V_END);
        rd_kafka_flush(m_rk, 3000);
    }
}

////////////////////////////////////////////////////////////////////////////////
// Factory
////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<IKv8Producer> IKv8Producer::Create(const Kv8Config &cfg)
{
    return std::make_unique<Kv8ProducerImpl>(cfg);
}

} // namespace kv8
