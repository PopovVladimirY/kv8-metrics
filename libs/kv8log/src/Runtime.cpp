////////////////////////////////////////////////////////////////////////////////
// kv8log/src/Runtime.cpp
//
// Loads kv8log_runtime.so / kv8log_runtime.dll at first use, resolves the
// plain-C vtable, and manages the per-process configuration state.
//
// Configuration resolution order:
//   1. Runtime::Configure() explicit override (call before first Add).
//   2. Command-line arguments: /KV8.brokers= /KV8.channel= /KV8.user= /KV8.pass=
//   3. Environment variables: KV8_BROKERS  KV8_CHANNEL  KV8_USER  KV8_PASS
//   4. Compiled-in defaults.
////////////////////////////////////////////////////////////////////////////////

#include "kv8log/Runtime.h"

#include <mutex>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <cstdio>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <shellapi.h>   // CommandLineToArgvW
#else
#  include <dlfcn.h>
#  include <unistd.h>    // readlink
#endif

namespace kv8log {

// Forward declarations for helpers defined in DefaultChannel.cpp.
extern void  FlushDefaultChannel(int timeout_ms);
extern void* GetDefaultChannelHandle();

// ── Internal state ─────────────────────────────────────────────────────────
namespace {

struct Config
{
    std::string brokers = "localhost:19092";
    std::string channel;   // filled in from exe name if empty
    std::string user    = "kv8producer";
    std::string pass    = "kv8secret";
};

struct State
{
    std::once_flag init_once;
    Config         config;
    bool           config_overridden = false;  // set by Runtime::Configure()
    Vtable         vtable{};
    void*          lib_handle = nullptr;       // dlopen / LoadLibrary handle
};

State& G()
{
    static State s;
    return s;
}

// ── Platform helpers ────────────────────────────────────────────────────

static std::string GetExeName()
{
#ifdef _WIN32
    wchar_t buf[512]{};
    GetModuleFileNameW(nullptr, buf, 511);
    // Convert wide to narrow (ASCII exe names only per project convention).
    char narrow[512]{};
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, narrow, 511, nullptr, nullptr);
    std::string path(narrow);
#elif defined(__linux__)
    char buf[512]{};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    std::string path = (n > 0) ? std::string(buf, (size_t)n) : "";
#else
    std::string path;
#endif
    // Strip directory and extension.
    auto slash = path.find_last_of("/\\");
    if (slash != std::string::npos) path = path.substr(slash + 1);
    auto dot = path.rfind('.');
    if (dot != std::string::npos) path = path.substr(0, dot);
    return path.empty() ? "kv8app" : path;
}

// Collect argv tokens from the OS without requiring main() to pass them.
static std::vector<std::string> GetArgv()
{
    std::vector<std::string> args;
#ifdef _WIN32
    int wargc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
        for (int i = 1; i < wargc; ++i) {
            char buf[512]{};
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, buf, 511, nullptr, nullptr);
            args.emplace_back(buf);
        }
        LocalFree(wargv);
    }
#elif defined(__linux__)
    FILE* f = fopen("/proc/self/cmdline", "rb");
    if (f) {
        char buf[4096];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';
        bool first = true;
        const char* p = buf;
        const char* end = buf + n;
        while (p < end) {
            size_t len = strlen(p);
            if (first) { first = false; } else { args.emplace_back(p); }
            p += len + 1;
        }
    }
#endif
    return args;
}

static const char* GetEnv(const char* name)
{
    const char* v = std::getenv(name);
    return (v && *v) ? v : nullptr;
}

// Resolve one "key=value" argument. Returns true + sets *out if matched.
static bool ParseArg(const std::string& arg, const char* key, std::string& out)
{
    if (arg.size() > strlen(key) && arg.compare(0, strlen(key), key) == 0) {
        out = arg.substr(strlen(key));
        return true;
    }
    return false;
}

static void ParseConfigFromArgvEnv(Config& cfg)
{
    // Command-line has priority over env.
    auto argv = GetArgv();
    for (const auto& a : argv) {
        ParseArg(a, "/KV8.brokers=", cfg.brokers);
        ParseArg(a, "/KV8.channel=", cfg.channel);
        ParseArg(a, "/KV8.user=",    cfg.user);
        ParseArg(a, "/KV8.pass=",    cfg.pass);
    }
    // Env vars fill in anything still at default.
    if (const char* v = GetEnv("KV8_BROKERS")) cfg.brokers = v;
    if (const char* v = GetEnv("KV8_CHANNEL")) cfg.channel = v;
    if (const char* v = GetEnv("KV8_USER"))    cfg.user    = v;
    if (const char* v = GetEnv("KV8_PASS"))    cfg.pass    = v;
    // Default channel from exe name.
    if (cfg.channel.empty())
        cfg.channel = "kv8log/" + GetExeName();
}

// ── Shared library loading ──────────────────────────────────────────────

#ifdef _WIN32
static void* LoadSym(void* lib, const char* name)
{
    return (void*)GetProcAddress((HMODULE)lib, name);
}
static void* OpenLib(const char* name)
{
    return (void*)LoadLibraryA(name);
}
#else
static void* LoadSym(void* lib, const char* name)
{
    return dlsym(lib, name);
}
static void* OpenLib(const char* name)
{
    return dlopen(name, RTLD_NOW | RTLD_LOCAL);
}
#endif

static bool LoadVtable(void* lib, Vtable& v)
{
#define SYM(field, sym) \
    v.field = reinterpret_cast<decltype(v.field)>(LoadSym(lib, sym)); \
    if (!v.field) { fprintf(stderr, "[kv8log] symbol missing: %s\n", sym); return false; }

    SYM(open,               "kv8log_open")
    SYM(close,              "kv8log_close")
    SYM(register_counter,   "kv8log_register_counter")
    SYM(add,                "kv8log_add")
    SYM(add_ts,             "kv8log_add_ts")
    SYM(flush,              "kv8log_flush")
    SYM(monotonic_to_ns,    "kv8log_monotonic_to_ns")
#undef SYM
    // Optional symbol: present in runtime versions that support remote enable/disable.
    v.set_counter_enabled = reinterpret_cast<decltype(v.set_counter_enabled)>(
        LoadSym(lib, "kv8log_set_counter_enabled"));
    // Optional UDT symbols: present in runtime versions that support UDT feeds.
    v.register_udt_schema = reinterpret_cast<decltype(v.register_udt_schema)>(
        LoadSym(lib, "kv8log_register_udt_schema"));
    v.register_udt_feed = reinterpret_cast<decltype(v.register_udt_feed)>(
        LoadSym(lib, "kv8log_register_udt_feed"));
    v.add_udt = reinterpret_cast<decltype(v.add_udt)>(
        LoadSym(lib, "kv8log_add_udt"));
    v.add_udt_ts = reinterpret_cast<decltype(v.add_udt_ts)>(
        LoadSym(lib, "kv8log_add_udt_ts"));
    // Optional trace log symbols (Phase L2).
    v.register_log_site = reinterpret_cast<decltype(v.register_log_site)>(
        LoadSym(lib, "kv8log_register_log_site"));
    v.log = reinterpret_cast<decltype(v.log)>(
        LoadSym(lib, "kv8log_log"));
    return true;
}

static void InitOnce()
{
    State& g = G();
    if (!g.config_overridden)
        ParseConfigFromArgvEnv(g.config);

    // Try loading the runtime shared library. Absent library is not an error.
    const char* lib_names[] = {
#ifdef _WIN32
        "kv8log_runtime.dll",
#elif defined(__APPLE__)
        "libkv8log_runtime.dylib",
        "./libkv8log_runtime.dylib",
#else
        "libkv8log_runtime.so",
        "./libkv8log_runtime.so",
#endif
    };

    for (const char* name : lib_names) {
        g.lib_handle = OpenLib(name);
        if (g.lib_handle) break;
    }

    if (!g.lib_handle) {
        // Normal operation: runtime not present, all Add() calls silently no-op.
        return;
    }

    if (!LoadVtable(g.lib_handle, g.vtable)) {
        // Partially-loaded library -- zero out so callers hit nullptr checks.
        g.vtable = Vtable{};
    }
}

} // anon namespace

// ── Runtime public API ──────────────────────────────────────────────────────

void Runtime::Configure(const char* brokers, const char* channel,
                        const char* user, const char* pass)
{
    State& g = G();
    // Must be called before first use; if init_once already fired, this is a no-op.
    g.config.brokers = brokers ? brokers : "localhost:19092";
    g.config.channel = channel ? channel : "";
    if (user) g.config.user = user;
    if (pass) g.config.pass = pass;
    g.config_overridden = true;
}

const Vtable& Runtime::Fn()
{
    std::call_once(G().init_once, InitOnce);
    return G().vtable;
}

void Runtime::Flush(int timeout_ms)
{
    // Flush is a best-effort broadcast across all open channel handles.
    // Individual channels call flush through their own handles.
    // This global flush is a convenience -- callers of the macro KV8_TEL_FLUSH().
    // Each Channel calls flush on its own handle; Runtime::Flush provides a
    // single call-site equivalent for applications that use only the default channel.
    //
    // Since Channel handles are not tracked globally here (channels are static
    // objects owned by user TUs), we invoke flush on the default channel handle
    // if it was opened.
    const Vtable& fn = Fn();
    if (!fn.flush) return;
    // DefaultChannel() is defined in DefaultChannel.cpp; it manages its own handle.
    // Access it via the Channel::Handle() indirection to avoid circular include.
    // The macro KV8_TEL_FLUSH() expands to Runtime::Flush() which is used primarily
    // with the default channel. Multi-channel users should call channel.Flush() directly.
    FlushDefaultChannel(timeout_ms);
}

uint64_t Runtime::MonotonicToNs(uint64_t mono_ns)
{
    const Vtable& fn = Fn();
    if (!fn.monotonic_to_ns) return 0;
    // The handle is needed; use the default channel handle.
    void* h = GetDefaultChannelHandle();
    if (!h) return 0;
    return fn.monotonic_to_ns(h, mono_ns);
}

uint32_t Runtime::RegisterLogSite(const char* file, uint16_t file_len,
                                  const char* func, uint16_t func_len,
                                  uint32_t    line,
                                  const char* fmt,  uint16_t fmt_len)
{
    const Vtable& fn = Fn();
    if (!fn.register_log_site) return 1u;       // runtime absent: benign sentinel
    void* h = GetDefaultChannelHandle();
    if (!h) return 1u;
    uint32_t r = fn.register_log_site(h, file, file_len, func, func_len,
                                       line, fmt, fmt_len);
    return r ? r : 1u;
}

void Runtime::Log(uint32_t site_hash, uint8_t level,
                  const void* payload, uint16_t payload_len,
                  uint8_t flags)
{
    const Vtable& fn = Fn();
    if (!fn.log) return;
    void* h = GetDefaultChannelHandle();
    if (!h) return;
    fn.log(h, site_hash, level, payload, payload_len, flags);
}

// ── Config accessor (used by Channel.cpp) ──────────────────────────────────
const char* RuntimeBrokers()  { std::call_once(G().init_once, InitOnce); return G().config.brokers.c_str(); }
const char* RuntimeChannel()  { std::call_once(G().init_once, InitOnce); return G().config.channel.c_str(); }
const char* RuntimeUser()     { std::call_once(G().init_once, InitOnce); return G().config.user.c_str(); }
const char* RuntimePass()     { std::call_once(G().init_once, InitOnce); return G().config.pass.c_str(); }

} // namespace kv8log
