////////////////////////////////////////////////////////////////////////////////
// kv8zoom/WsServer.cpp
////////////////////////////////////////////////////////////////////////////////

#include "WsServer.h"

// uWebSockets v20 + uSockets
#include <App.h>          // uWS::App (comes from uWebSockets include path)
#include <libusockets.h>  // us_create_timer, us_timer_set, us_timer_ext, ...

#include <cstdio>
#include <vector>

namespace kv8zoom {

// ── Timer trampoline helpers ─────────────────────────────────────────────────

struct TimerCtx {
    WsServer::TimerFn fn;
};

static void TimerCallback(struct us_timer_t* t)
{
    auto* ctx = reinterpret_cast<TimerCtx*>(us_timer_ext(t));
    ctx->fn();
}

// ── Impl (holds the uWS::App instance) ──────────────────────────────────────

struct WsServer::Impl {
    uWS::App app;
    std::vector<us_timer_t*> timers;
};

// ── WsServer ─────────────────────────────────────────────────────────────────

WsServer::WsServer()
    : m_impl(new Impl())
{
}

WsServer::~WsServer()
{
    // Timers are owned by the uSockets event loop; they are freed automatically
    // when the loop is drained.
    delete m_impl;
}

WsServer& WsServer::AddTimer(TimerFn fn, int intervalMs)
{
    m_pendingTimers.push_back({std::move(fn), intervalMs});
    return *this;
}

void WsServer::Run(uint16_t port)
{
    auto& app = m_impl->app;

    // Wire up WebSocket handlers.
    app.ws<SocketData>("/*", {
        .open = [this](WsType* ws) {
            m_clients.insert(ws);
            ws->subscribe("broadcast");
            if (m_onOpen) m_onOpen(ws);
        },
        .message = [this](WsType* ws, std::string_view msg, uWS::OpCode /*op*/) {
            if (m_onMessage) m_onMessage(ws, msg);
        },
        .close = [this](WsType* ws, int /*code*/, std::string_view /*reason*/) {
            m_clients.erase(ws);
            if (m_onClose) m_onClose(ws);
        }
    }).listen(port, [this, port](auto* sock) {
        m_listenSock = sock;
        if (sock) {
            fprintf(stderr, "[WsServer] Listening on ws://0.0.0.0:%u\n", (unsigned)port);
        } else {
            fprintf(stderr, "[WsServer] ERROR: failed to bind port %u\n", (unsigned)port);
        }
    });

    // Install periodic timers now that the loop exists.
    // uWS::Loop IS the us_loop_t (it's cast-compatible with the C type).
    us_loop_t* loop = reinterpret_cast<us_loop_t*>(uWS::Loop::get());
    for (auto& pt : m_pendingTimers) {
        // Allocate space for TimerCtx in the uSockets timer extension.
        us_timer_t* t = us_create_timer(loop, 0, static_cast<unsigned>(sizeof(TimerCtx)));
        TimerCtx* ctx = reinterpret_cast<TimerCtx*>(us_timer_ext(t));
        new (ctx) TimerCtx{pt.fn}; // placement new
        us_timer_set(t, TimerCallback,
                     static_cast<unsigned>(pt.intervalMs),
                     static_cast<unsigned>(pt.intervalMs));
        m_impl->timers.push_back(t);
    }

    app.run(); // blocks until us_loop_free() or Stop()
}

void WsServer::Stop()
{
    if (m_listenSock) {
        Defer([this]() {
            us_listen_socket_close(0, m_listenSock);
            m_listenSock = nullptr;
        });
    }
}

void WsServer::BroadcastText(std::string_view msg)
{
    m_impl->app.publish("broadcast", msg, uWS::OpCode::TEXT);
}

void WsServer::Defer(std::function<void()> fn)
{
    uWS::Loop::get()->defer(std::move(fn));
}

} // namespace kv8zoom
