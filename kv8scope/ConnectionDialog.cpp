// kv8scope -- Kv8 Software Oscilloscope
// ConnectionDialog.cpp -- Modal connection settings dialog implementation.

#include "ConnectionDialog.h"
#include "ConfigStore.h"

#include <kv8/IKv8Consumer.h>
#include <kv8/Kv8Types.h>

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Find the index of @p sValue in @p ppItems (case-insensitive).
/// Returns 0 if not found.
static int FindComboIndex(const char* const* ppItems, int iCount,
                          const std::string& sValue)
{
    for (int i = 0; i < iCount; ++i)
    {
#ifdef _MSC_VER
        if (_stricmp(ppItems[i], sValue.c_str()) == 0)
#else
        if (strcasecmp(ppItems[i], sValue.c_str()) == 0)
#endif
            return i;
    }
    return 0;
}

/// Copy a std::string into a fixed char buffer, ensuring null termination.
static void CopyToBuffer(char* pDst, size_t nDstSize,
                          const std::string& sSrc)
{
    size_t n = (std::min)(sSrc.size(), nDstSize - 1);
    std::memcpy(pDst, sSrc.c_str(), n);
    pDst[n] = '\0';
}

// ---------------------------------------------------------------------------
// Open -- snapshot current config into edit buffers
// ---------------------------------------------------------------------------

void ConnectionDialog::Open()
{
    if (!m_pConfig)
        return;

    const ScopeConfig& cfg = m_pConfig->Get();

    CopyToBuffer(m_aBrokers,  sizeof(m_aBrokers),  cfg.sBrokers);
    CopyToBuffer(m_aUsername, sizeof(m_aUsername), cfg.sUsername);
    CopyToBuffer(m_aPassword, sizeof(m_aPassword), cfg.sPassword);

    m_iSecProto = FindComboIndex(kSecurityProtocols, kNumSecProto,
                                 cfg.sSecurityProtocol);
    m_iSaslMech = FindComboIndex(kSaslMechanisms, kNumSaslMech,
                                 cfg.sSaslMechanism);
    m_iPollMs   = cfg.iSessionPollMs;

    ResetTestState();

    m_bOpen       = true;
    m_bJustOpened = true;
}

// ---------------------------------------------------------------------------
// ResetTestState
// ---------------------------------------------------------------------------

void ConnectionDialog::ResetTestState()
{
    m_eTestState  = TestState::Idle;
    m_sTestResult.clear();
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void ConnectionDialog::Render()
{
    if (!m_bOpen)
        return;

    // Open the popup on the first frame after Open() was called.
    if (m_bJustOpened)
    {
        ImGui::OpenPopup("Connection Settings");
        m_bJustOpened = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(480.0f, 0.0f), ImGuiCond_Appearing);

    if (!ImGui::BeginPopupModal("Connection Settings", &m_bOpen,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    // ── Input fields ─────────────────────────────────────────────────
    const float fLabelW = 140.0f;

    ImGui::AlignTextToFramePadding();
    ImGui::Text("Brokers");
    ImGui::SameLine(fLabelW);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##Brokers", m_aBrokers, sizeof(m_aBrokers));

    ImGui::AlignTextToFramePadding();
    ImGui::Text("Security protocol");
    ImGui::SameLine(fLabelW);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::Combo("##SecProto", &m_iSecProto,
                 kSecurityProtocols, kNumSecProto);

    ImGui::AlignTextToFramePadding();
    ImGui::Text("SASL mechanism");
    ImGui::SameLine(fLabelW);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::Combo("##SaslMech", &m_iSaslMech,
                 kSaslMechanisms, kNumSaslMech);

    ImGui::AlignTextToFramePadding();
    ImGui::Text("Username");
    ImGui::SameLine(fLabelW);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##Username", m_aUsername, sizeof(m_aUsername));

    ImGui::AlignTextToFramePadding();
    ImGui::Text("Password");
    ImGui::SameLine(fLabelW);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##Password", m_aPassword, sizeof(m_aPassword),
                     ImGuiInputTextFlags_Password);

    ImGui::AlignTextToFramePadding();
    ImGui::Text("Poll interval (ms)");
    ImGui::SameLine(fLabelW);
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("##PollMs", &m_iPollMs, 500, 1000);
    if (m_iPollMs < 500)
        m_iPollMs = 500;

    // ── Test Connection ──────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Test Connection"))
    {
        m_eTestState = TestState::Running;
        m_sTestResult.clear();

        // Build a temporary Kv8Config from the edit buffers.
        kv8::Kv8Config testCfg;
        testCfg.sBrokers       = m_aBrokers;
        testCfg.sSecurityProto = kSecurityProtocols[m_iSecProto];
        testCfg.sSaslMechanism = kSaslMechanisms[m_iSaslMech];
        testCfg.sUser          = m_aUsername;
        testCfg.sPass          = m_aPassword;
        testCfg.sGroupID.clear();

        try
        {
            auto pConsumer = kv8::IKv8Consumer::Create(testCfg);
            auto channels  = pConsumer->ListChannels(3000);

            m_eTestState  = TestState::Success;
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "OK -- %d channel(s) found.",
                          static_cast<int>(channels.size()));
            m_sTestResult = buf;
        }
        catch (const std::exception& ex)
        {
            m_eTestState  = TestState::Failed;
            m_sTestResult = ex.what();
        }
        catch (...)
        {
            m_eTestState  = TestState::Failed;
            m_sTestResult = "Unknown error";
        }
    }

    // Show test result on the same line.
    ImGui::SameLine();
    switch (m_eTestState)
    {
    case TestState::Running:
        ImGui::TextColored(ImVec4(0.90f, 0.90f, 0.30f, 1.0f),
                           "Testing...");
        break;
    case TestState::Success:
        ImGui::TextColored(ImVec4(0.0f, 0.83f, 0.67f, 1.0f),  // #00d4aa
                           "%s", m_sTestResult.c_str());
        break;
    case TestState::Failed:
        ImGui::TextColored(ImVec4(0.84f, 0.37f, 0.0f, 1.0f),  // #d55e00
                           "%s", m_sTestResult.c_str());
        break;
    case TestState::Idle:
    default:
        break;
    }

    // ── OK / Cancel ──────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    bool bAccepted = false;

    if (ImGui::Button("OK", ImVec2(100.0f, 0.0f)))
    {
        bAccepted = true;
    }

    ImGui::SameLine();

    if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f)))
    {
        m_bOpen = false;
        ImGui::CloseCurrentPopup();
    }

    // Apply on OK -- write edit buffers back to ConfigStore.
    if (bAccepted && m_pConfig)
    {
        ScopeConfig& cfg = m_pConfig->GetMut();

        cfg.sBrokers          = m_aBrokers;
        cfg.sSecurityProtocol = kSecurityProtocols[m_iSecProto];
        cfg.sSaslMechanism    = kSaslMechanisms[m_iSaslMech];
        cfg.sUsername          = m_aUsername;
        cfg.sPassword          = m_aPassword;
        cfg.iSessionPollMs    = m_iPollMs;

        m_pConfig->MarkDirty();
        m_pConfig->Save();

        m_bOpen = false;
        ImGui::CloseCurrentPopup();

        if (m_onAccept)
            m_onAccept();
    }

    ImGui::EndPopup();
}
