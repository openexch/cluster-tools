#include "genesis/GenesisSettings.h"

#include <charconv>
#include <limits>

#include "cluster_tools/runtime/DriverTimeout.h"

namespace cluster_tools { namespace app { namespace genesis {

const char *driverModeName(DriverMode mode) noexcept
{
    switch (mode)
    {
        case DriverMode::UNSET: return "unset";
        case DriverMode::EMBEDDED: return "embedded";
        case DriverMode::EXTERNAL: return "external";
    }
    return "?";
}

namespace {

std::string trimmed(const char *raw)
{
    if (nullptr == raw)
    {
        return {};
    }
    std::string s(raw);
    const auto first = s.find_first_not_of(" \t");
    if (std::string::npos == first)
    {
        return {};
    }
    const auto last = s.find_last_not_of(" \t");
    return s.substr(first, last - first + 1);
}

std::string required(const EnvGetter &getenv, const char *key, std::vector<std::string> &e, const char *what)
{
    std::string v = trimmed(getenv(key));
    if (v.empty())
    {
        e.push_back(std::string(key) + " is required: " + what);
    }
    return v;
}

// An optional integer key with a default and a range; a present value that does not parse refuses the start.
std::int64_t integer(const EnvGetter &getenv, const char *key, std::int64_t fallback, std::int64_t min, std::int64_t max,
    std::vector<std::string> &e)
{
    const std::string v = trimmed(getenv(key));
    if (v.empty())
    {
        return fallback;
    }
    std::int64_t out = 0;
    const auto parsed = std::from_chars(v.data(), v.data() + v.size(), out);
    if (parsed.ec != std::errc() || parsed.ptr != v.data() + v.size() || '+' == v.front() || out < min || out > max)
    {
        e.push_back(std::string(key) + " must be an integer in " + std::to_string(min) + ".." + std::to_string(max) +
            ", got '" + v + "'");
        return fallback;
    }
    return out;
}

client::CoreCredentials credential(const EnvGetter &getenv, const char *key, client::SessionClass cls,
    std::vector<std::string> &e)
{
    const std::string spec = trimmed(getenv(key));
    if (spec.empty())
    {
        e.push_back(std::string(key) + " is required: keyId:" + client::nameOf(cls) + ":secretHex");
        return {};
    }
    client::CredentialsResult r = client::parseCoreCredentials(spec);
    if (!r.ok())
    {
        e.push_back(std::string(key) + ": " + r.error);
        return {};
    }
    if (r.credentials.sessionClass != cls)
    {
        e.push_back(std::string(key) + " must be a " + client::nameOf(cls) + " key, got " +
            client::nameOf(r.credentials.sessionClass));
        return {};
    }
    return r.credentials;
}

}

client::ClientSettings clientSettings(const GenesisSettings &s, const std::string &ingress,
    const client::CoreCredentials &credentials, const char *name)
{
    client::ClientSettings c;
    c.ingressEndpoints = ingress;
    c.ingressChannel = s.ingressChannel;
    c.responseChannel = s.responseChannel;
    c.credentials = credentials.encode();
    c.clientName = name;
    return c;
}

SettingsResult parseGenesisSettings(const EnvGetter &getenv, bool requireTool, const runtime::DirProbe &probe)
{
    SettingsResult r;
    GenesisSettings &s = r.settings;
    std::vector<std::string> &e = r.errors;

    const std::string mode = trimmed(getenv("GENESIS_DRIVER_MODE"));
    if ("embedded" == mode)
    {
        s.driverMode = DriverMode::EMBEDDED;
    }
    else if ("external" == mode)
    {
        s.driverMode = DriverMode::EXTERNAL;
    }
    else if (mode.empty())
    {
        e.push_back("GENESIS_DRIVER_MODE is required (no default): embedded (dev and demo) or external (prod, the host "
            "aeronmd)");
    }
    else
    {
        e.push_back("GENESIS_DRIVER_MODE must be exactly embedded or external, got '" + mode + "'");
    }
    const bool aeronDirSet = nullptr != getenv("AERON_DIR");
    const bool driverDirSet = nullptr != getenv("GENESIS_DRIVER_DIR");
    const bool instanceSet = nullptr != getenv("GENESIS_INSTANCE_ID");
    if (DriverMode::EXTERNAL == s.driverMode)
    {
        s.aeronDir = trimmed(getenv("AERON_DIR"));
        if (s.aeronDir.empty())
        {
            e.push_back("AERON_DIR is required with GENESIS_DRIVER_MODE=external (the host aeronmd's directory)");
        }
        else if (!probe.isDirectory(s.aeronDir))
        {
            e.push_back("AERON_DIR " + s.aeronDir + " is not an existing directory (is the host aeronmd running?)");
        }
        if (driverDirSet)
        {
            e.push_back("GENESIS_DRIVER_DIR is set but GENESIS_DRIVER_MODE=external uses the host aeronmd at AERON_DIR: "
                "remove it");
        }
        if (instanceSet)
        {
            e.push_back("GENESIS_INSTANCE_ID is set but GENESIS_DRIVER_MODE=external starts no driver: remove it");
        }
    }
    else if (DriverMode::EMBEDDED == s.driverMode)
    {
        s.driverDir = trimmed(getenv("GENESIS_DRIVER_DIR"));
        if (s.driverDir.empty())
        {
            e.push_back("GENESIS_DRIVER_DIR is required with GENESIS_DRIVER_MODE=embedded (the parent of the per-instance "
                "driver dir)");
        }
        else if ('/' != s.driverDir.front())
        {
            e.push_back("GENESIS_DRIVER_DIR must be an absolute path, got '" + s.driverDir + "'");
        }
        else if (!probe.isDirectory(s.driverDir))
        {
            e.push_back("GENESIS_DRIVER_DIR " + s.driverDir + " is not an existing directory");
        }
        else if (!probe.isWritable(s.driverDir))
        {
            e.push_back("GENESIS_DRIVER_DIR " + s.driverDir + " is not writable");
        }
        if (aeronDirSet)
        {
            e.push_back("AERON_DIR is set but GENESIS_DRIVER_MODE=embedded starts its own driver: remove it");
        }
        s.instanceId = integer(getenv, "GENESIS_INSTANCE_ID", DEFAULT_INSTANCE_ID, 1,
            std::numeric_limits<std::int64_t>::max(), e);
    }
    if (const char *raw = getenv("GENESIS_DRIVER_TIMEOUT_MS"); nullptr != raw && !trimmed(raw).empty())
    {
        std::string error;
        const std::string text = trimmed(raw);
        if (const auto v = runtime::parseDriverTimeoutMs("GENESIS_DRIVER_TIMEOUT_MS", text.c_str(), error))
        {
            s.driverTimeoutMs = *v;
        }
        else
        {
            e.push_back(error);
        }
    }

    s.coreIngress = required(getenv, "GENESIS_CORE_INGRESS", e, "the core members, id=host:port,...");
    s.meIngress = required(getenv, "GENESIS_ME_INGRESS", e, "the ME members, id=host:port,...");
    s.responseChannel = required(getenv, "GENESIS_RESPONSE_CHANNEL", e,
        "the egress channel the clusters answer on, e.g. aeron:udp?endpoint=10.0.0.5:0");
    if (const std::string v = trimmed(getenv("GENESIS_INGRESS_CHANNEL")); !v.empty())
    {
        s.ingressChannel = v;
    }
    s.coreAdmin = credential(getenv, "GENESIS_CORE_ADMIN_CREDENTIAL", client::SessionClass::ADMIN, e);
    if (requireTool || nullptr != getenv("GENESIS_CORE_TOOL_CREDENTIAL"))
    {
        s.coreTool = credential(getenv, "GENESIS_CORE_TOOL_CREDENTIAL", client::SessionClass::TOOL, e);
        s.coreToolSet = true;
    }
    s.meAdmin = credential(getenv, "GENESIS_ME_ADMIN_CREDENTIAL", client::SessionClass::ADMIN, e);
    s.activationRecord = required(getenv, "GENESIS_ACTIVATION_RECORD", e,
        "the activation record file (activate writes it; status and verify read it)");
    if (!s.activationRecord.empty())
    {
        const auto slash = s.activationRecord.rfind('/');
        const std::string parent = std::string::npos == slash ? "." : 0 == slash ? "/" : s.activationRecord.substr(0, slash);
        if (!probe.isDirectory(parent))
        {
            e.push_back("GENESIS_ACTIVATION_RECORD " + s.activationRecord + ": the directory " + parent + " does not exist");
        }
    }

    constexpr std::int64_t HOUR_MS = 3'600'000;
    Timeouts &t = s.timeouts;
    t.connectMs = integer(getenv, "GENESIS_CONNECT_TIMEOUT_MS", DEFAULT_CONNECT_TIMEOUT_MS, 1'000, HOUR_MS, e);
    t.replyMs = integer(getenv, "GENESIS_REPLY_TIMEOUT_MS", DEFAULT_REPLY_TIMEOUT_MS, 100, HOUR_MS, e);
    t.meEpochMs = integer(getenv, "GENESIS_ME_EPOCH_TIMEOUT_MS", DEFAULT_ME_EPOCH_TIMEOUT_MS, 1'000, HOUR_MS, e);
    t.gateOpenRetries = integer(getenv, "GENESIS_GATE_OPEN_RETRIES", DEFAULT_GATE_OPEN_RETRIES, 1, 1'000'000, e);
    t.gateOpenRetryIntervalMs = integer(getenv, "GENESIS_GATE_OPEN_RETRY_INTERVAL_MS",
        DEFAULT_GATE_OPEN_RETRY_INTERVAL_MS, 0, 60'000, e);
    t.journalArmMs = integer(getenv, "GENESIS_JOURNAL_ARM_TIMEOUT_MS", DEFAULT_JOURNAL_ARM_TIMEOUT_MS, 100, HOUR_MS, e);

    if (e.empty())
    {
        if (const std::string error = client::validate(clientSettings(s, s.coreIngress, s.coreAdmin, "ct-genesis"));
            !error.empty())
        {
            e.push_back("core: " + error);
        }
        if (const std::string error = client::validate(clientSettings(s, s.meIngress, s.meAdmin, "ct-genesis"));
            !error.empty())
        {
            e.push_back("ME: " + error);
        }
    }
    return r;
}

}}}
