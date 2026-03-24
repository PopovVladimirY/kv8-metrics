////////////////////////////////////////////////////////////////////////////////
// kv8zoom/main.cpp
////////////////////////////////////////////////////////////////////////////////

#include "Config.h"
#include "ZoomBridge.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace {

kv8zoom::ZoomBridge* g_bridge = nullptr;

void HandleSignal(int /*sig*/)
{
    if (g_bridge) {
        fprintf(stderr, "\n[kv8zoom] Shutting down...\n");
        g_bridge->Stop();
    }
}

} // namespace

int main(int argc, char* argv[])
{
    std::string configPath = "kv8zoom.json";
    if (argc >= 2) configPath = argv[1];

    kv8zoom::Config cfg;
    try {
        cfg = kv8zoom::Config::LoadFromFile(configPath);
        fprintf(stderr, "[kv8zoom] Loaded config from '%s'\n", configPath.c_str());
    } catch (const std::exception& e) {
        fprintf(stderr, "[kv8zoom] %s -- using built-in defaults\n", e.what());
        cfg = kv8zoom::Config::Defaults();
    }

    fprintf(stderr, "[kv8zoom] Broker:  %s\n", cfg.kafka.brokers.c_str());
    fprintf(stderr, "[kv8zoom] Channel: %s\n", cfg.kafka.channel.c_str());
    fprintf(stderr, "[kv8zoom] WS port: %u\n", (unsigned)cfg.bridge.ws_port);
    fprintf(stderr, "\n");
    fprintf(stderr, "  WebSocket bridge : ws://localhost:%u\n", (unsigned)cfg.bridge.ws_port);
    fprintf(stderr, "\n");
    fprintf(stderr, "  To open the UI, run in a separate terminal:\n");
    fprintf(stderr, "    cd kv8zoom/frontend\n");
    fprintf(stderr, "    npm install        (first time only)\n");
    fprintf(stderr, "    npm run dev\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  Then open: http://localhost:5173\n");
    fprintf(stderr, "\n");

    // ZoomBridge owns KafkaReader which contains ~6 MB of SPSC ring buffers.
    // Allocating it on the stack would overflow the default 1 MB Windows stack
    // at the __chkstk prologue before a single line of main() runs.
    auto bridge = std::make_unique<kv8zoom::ZoomBridge>(cfg);
    g_bridge = bridge.get();

    std::signal(SIGINT,  HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    bridge->Run(); // blocks

    fprintf(stderr, "[kv8zoom] Exited cleanly.\n");
    return 0;
}
