////////////////////////////////////////////////////////////////////////////////
// kv8zoom/WsServer.h
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>

// Forward-declare uWebSockets types to avoid pulling heavy headers into every
// translation unit that includes this file.
struct us_listen_socket_t;

namespace uWS {
template <bool SSL, bool isServer, typename USERDATA>
struct WebSocket;
template <bool SSL>
struct TemplatedApp;
} // namespace uWS

namespace kv8zoom {

/// uWebSockets v20 WebSocket server (plaintext).
///
/// Threading:
///   Run() blocks the calling thread in the uWS event loop.
///   BroadcastText() and Defer() may be called from any thread, but
///   BroadcastText() must only be invoked from within a Defer() callback or
///   other uWS loop callback to stay single-threaded.
///
/// The flow for the Kafka consumer thread is:
///   consumer thread -> ring buffer -> uWS timer fires -> BroadcastText()
class WsServer
{
public:
    /// Per-socket user data (opaque to WsServer; available to the message callback).
    struct SocketData {};

    using WsType      = uWS::WebSocket<false, true, SocketData>;
    using OnOpenFn    = std::function<void(WsType*)>;
    using OnMessageFn = std::function<void(WsType*, std::string_view)>;
    using OnCloseFn   = std::function<void(WsType*)>;
    using TimerFn     = std::function<void()>;

    WsServer();
    ~WsServer();

    // Register callbacks before calling Run().
    WsServer& OnOpen(OnOpenFn fn)       { m_onOpen    = std::move(fn); return *this; }
    WsServer& OnMessage(OnMessageFn fn) { m_onMessage = std::move(fn); return *this; }
    WsServer& OnClose(OnCloseFn fn)     { m_onClose   = std::move(fn); return *this; }

    /// Register a periodic callback invoked on the event-loop thread.
    /// @param intervalMs  Repeat interval in milliseconds.
    WsServer& AddTimer(TimerFn fn, int intervalMs);

    /// Start the event loop on the calling thread. Blocks until Stop() is called.
    void Run(uint16_t port);

    /// Signal the event loop to exit. Thread-safe.
    void Stop();

    /// Send a text message to every connected client.
    /// MUST be called from the event-loop thread (e.g. inside a timer callback).
    void BroadcastText(std::string_view msg);

    /// Schedule a callable to run on the event-loop thread.
    /// Safe to call from any thread.
    void Defer(std::function<void()> fn);

    size_t ConnectionCount() const noexcept { return m_clients.size(); }

private:
    // Pimpl avoids exposing uWS template types to every consumer.
    struct Impl;
    Impl* m_impl = nullptr;

    OnOpenFn    m_onOpen;
    OnMessageFn m_onMessage;
    OnCloseFn   m_onClose;

    struct PendingTimer {
        TimerFn fn;
        int     intervalMs;
    };
    std::vector<PendingTimer> m_pendingTimers;

    std::unordered_set<WsType*> m_clients;
    us_listen_socket_t*         m_listenSock = nullptr;
};

} // namespace kv8zoom
