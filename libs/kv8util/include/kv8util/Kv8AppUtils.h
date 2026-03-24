////////////////////////////////////////////////////////////////////////////////
// kv8util/Kv8AppUtils.h -- cross-platform application utilities
//
// Provides common building blocks shared across kv8 tools, with no file-scope
// global variables.
//
//   AppSignal   -- cooperative shutdown via Ctrl+C / SIGINT / SIGTERM.
//                  Uses a Meyer's singleton so the running flag lives inside
//                  a function-local static, not a file-scope global.
//
//   CheckEscKey -- non-blocking stdin check for the Esc key (0x1B).
//
//   BuildKv8Config -- construct a kv8::Kv8Config from common CLI parameters.
//
// Thread safety
// -------------
//   AppSignal::Install() must be called once in main() before any concurrent
//   use.  After that, IsRunning() and RequestStop() are safe from any thread.
//   CheckEscKey() touches stdin -- call from one thread only.
////////////////////////////////////////////////////////////////////////////////

#pragma once

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <Windows.h>
#  include <conio.h>
#else
#  include <signal.h>
#  include <poll.h>
#  include <unistd.h>
#endif

#include <atomic>
#include <string>

#include <kv8/Kv8Types.h>

namespace kv8util {

////////////////////////////////////////////////////////////////////////////////
// AppSignal -- cooperative shutdown through OS signal handlers
//
// Usage:
//   kv8util::AppSignal::Install();                // once in main()
//   while (kv8util::AppSignal::IsRunning()) { }   // main loop condition
//   kv8util::AppSignal::RequestStop();             // programmatic stop
////////////////////////////////////////////////////////////////////////////////

class AppSignal
{
public:
    /// Return the single instance (Meyer's singleton -- no file-scope global).
    static AppSignal& Instance()
    {
        static AppSignal s_instance;
        return s_instance;
    }

    /// Register OS-level Ctrl+C / SIGINT / SIGTERM handlers that call
    /// RequestStop().  Must be called once before the main loop.
    static void Install()
    {
#ifdef _WIN32
        SetConsoleCtrlHandler(CtrlHandler, TRUE);
#else
        struct sigaction sa{};
        sa.sa_handler = SigHandler;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT,  &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
#endif
    }

    /// True while the application should keep running.
    static bool IsRunning()
    {
        return Instance().m_bRunning.load(std::memory_order_relaxed);
    }

    /// Request a cooperative stop.  Called by signal handlers and by
    /// application code (e.g. on Esc key).
    static void RequestStop()
    {
        Instance().m_bRunning.store(false, std::memory_order_relaxed);
    }

private:
    AppSignal() = default;
    std::atomic<bool> m_bRunning{true};

#ifdef _WIN32
    static BOOL WINAPI CtrlHandler(DWORD)
    {
        Instance().RequestStop();
        return TRUE;
    }
#else
    static void SigHandler(int)
    {
        Instance().RequestStop();
    }
#endif
};

////////////////////////////////////////////////////////////////////////////////
// CheckEscKey -- non-blocking Esc key detection
//
// Returns true when Esc (0x1B) is detected on stdin.  Stateless.
////////////////////////////////////////////////////////////////////////////////

inline bool CheckEscKey()
{
#ifdef _WIN32
    if (_kbhit())
    {
        int ch = _getch();
        if (ch == 27)
            return true;
    }
#else
    struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
    if (poll(&pfd, 1, 0) > 0)
    {
        char c = 0;
        if (read(STDIN_FILENO, &c, 1) == 1 && c == 27)
            return true;
    }
#endif
    return false;
}

////////////////////////////////////////////////////////////////////////////////
// BuildKv8Config -- construct a kv8::Kv8Config from common CLI parameters
//
// Eliminates the repetitive 5-6 line Kv8Config construction that appears in
// every tool's main() and helper functions.
////////////////////////////////////////////////////////////////////////////////

inline kv8::Kv8Config BuildKv8Config(
    const std::string &sBrokers,
    const std::string &sSecurityProto,
    const std::string &sSaslMechanism,
    const std::string &sUser,
    const std::string &sPass,
    const std::string &sGroupID = "")
{
    kv8::Kv8Config cfg;
    cfg.sBrokers       = sBrokers;
    cfg.sSecurityProto = sSecurityProto;
    cfg.sSaslMechanism = sSaslMechanism;
    cfg.sUser          = sUser;
    cfg.sPass          = sPass;
    if (!sGroupID.empty())
        cfg.sGroupID = sGroupID;
    return cfg;
}

} // namespace kv8util
