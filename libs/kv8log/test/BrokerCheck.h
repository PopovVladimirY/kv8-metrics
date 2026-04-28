////////////////////////////////////////////////////////////////////////////////
// kv8log/test/BrokerCheck.h
//
// Phase L5 integration-test helper: try to reach the Kafka broker.  When the
// broker is unreachable the integration test exits with code 0 ("SKIP") so
// CI without Docker stays green -- broker absence is not a test failure.
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <kv8/IKv8Consumer.h>
#include <kv8util/Kv8AppUtils.h>

#include <cstdio>
#include <exception>
#include <string>

inline bool BrokerAvailable(const std::string& sBrokers,
                            const std::string& sUser,
                            const std::string& sPass)
{
    try
    {
        auto kCfg = kv8util::BuildKv8Config(
            sBrokers, "sasl_plaintext", "PLAIN", sUser, sPass, "");
        auto consumer = kv8::IKv8Consumer::Create(kCfg);
        if (!consumer) return false;
        // ListChannels does a metadata round-trip; failure surfaces as throw.
        (void)consumer->ListChannels(2000);
        return true;
    }
    catch (const std::exception& ex)
    {
        fprintf(stdout, "[SKIP] broker unreachable at %s: %s\n",
                sBrokers.c_str(), ex.what());
        return false;
    }
    catch (...)
    {
        fprintf(stdout, "[SKIP] broker unreachable at %s (unknown error)\n",
                sBrokers.c_str());
        return false;
    }
}
