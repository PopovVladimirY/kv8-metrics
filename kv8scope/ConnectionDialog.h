// kv8scope -- Kv8 Software Oscilloscope
// ConnectionDialog.h -- Modal connection settings dialog.
//
// Opened via File > Connect.  Edits a local copy of the connection
// fields; on OK the changes are written back to ConfigStore and
// SessionManager is restarted.

#pragma once

#include <functional>
#include <string>

class ConfigStore;

class ConnectionDialog
{
public:
    ConnectionDialog() = default;

    /// Callback invoked when the user presses OK and settings have been
    /// written to ConfigStore.  The owner (App) should restart
    /// SessionManager inside this callback.
    using OnAcceptFn = std::function<void()>;

    void SetBackends(ConfigStore* pConfig) { m_pConfig = pConfig; }
    void SetOnAccept(OnAcceptFn fn)        { m_onAccept = std::move(fn); }

    /// Open the dialog.  Copies current ConfigStore values into
    /// the local edit buffers.
    void Open();

    /// Render the modal popup.  Call every frame; the popup is only
    /// visible while m_bOpen is true.
    void Render();

    bool IsOpen() const { return m_bOpen; }

private:
    void ResetTestState();

    ConfigStore*  m_pConfig  = nullptr;
    OnAcceptFn    m_onAccept;

    bool m_bOpen        = false;
    bool m_bJustOpened   = false;  // triggers ImGui::OpenPopup once

    // ── Edit buffers (local copies while the dialog is open) ──
    char   m_aBrokers[512]   = {};
    int    m_iSecProto       = 0;   // index into kSecurityProtocols
    int    m_iSaslMech       = 0;   // index into kSaslMechanisms
    char   m_aUsername[128]  = {};
    char   m_aPassword[128]  = {};
    int    m_iPollMs         = 5000;

    // ── Test Connection state ──
    enum class TestState { Idle, Running, Success, Failed };
    TestState   m_eTestState  = TestState::Idle;
    std::string m_sTestResult;

    // ── Combo item arrays ──
    static constexpr const char* kSecurityProtocols[] = {
        "plaintext",
        "sasl_plaintext",
        "sasl_ssl"
    };
    static constexpr int kNumSecProto = 3;

    static constexpr const char* kSaslMechanisms[] = {
        "PLAIN",
        "SCRAM-SHA-256",
        "SCRAM-SHA-512"
    };
    static constexpr int kNumSaslMech = 3;
};
