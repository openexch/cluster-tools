#ifndef CLUSTER_TOOLS_ARCHIVE_ARCHIVE_SETTINGS_H
#define CLUSTER_TOOLS_ARCHIVE_ARCHIVE_SETTINGS_H

// Strict settings of the bridge's archive side (RC2 S9a contract 4.1; rc2-bridge-cpp 2.11 + the
// BRIDGE_JOURNAL_MTU amendment). parse() returns every error it finds and never a default: a
// missing or invalid key refuses the start (exit 78, section 7 row C1). Pure.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace cluster_tools { namespace archive {

struct ArchiveEndpoint
{
    std::string host;
    std::uint16_t port = 0;

    std::string text() const;
    bool operator==(const ArchiveEndpoint &) const = default;
};

struct ArchiveSettings
{
    std::vector<ArchiveEndpoint> archives; // BRIDGE_ME_JOURNAL_ARCHIVES, in failover order
    std::int32_t controlStreamId = 0;      // BRIDGE_JOURNAL_CONTROL_STREAM
    std::int32_t journalStreamId = 0;      // BRIDGE_JOURNAL_STREAM
    std::string journalChannel;            // BRIDGE_JOURNAL_CHANNEL
    std::int32_t replayStreamId = 0;       // BRIDGE_REPLAY_STREAM
    std::string localHost;                 // BRIDGE_LOCAL_HOST
    std::int32_t journalMtu = 0;           // BRIDGE_JOURNAL_MTU
    std::string driverDir;                 // BRIDGE_DRIVER_DIR (bridge process only)
};

struct ArchiveSettingsResult
{
    std::optional<ArchiveSettings> settings;
    std::vector<std::string> errors;

    bool ok() const noexcept { return settings.has_value(); }
};

enum class SettingsScope : std::uint8_t
{
    BRIDGE, // the bridge process: BRIDGE_DRIVER_DIR is required (it creates the per-pid driver dir)
    TOOL    // ct-journal-tail: an external driver, no BRIDGE_DRIVER_DIR
};

using SettingLookup = std::function<std::optional<std::string>(const std::string &key)>;

// Every key named in 4.1, plus the refused BRIDGE_HALT_ON_GAP (cpp: F5).
ArchiveSettingsResult parseArchiveSettings(const SettingLookup &lookup, SettingsScope scope);

// localhost, every 127/8 form inet_aton accepts, ::1 and ::ffff:127.x (review N-12).
bool isLoopbackHost(const std::string &host) noexcept;
// The comparison form of a host: canonical IPv4/IPv6 text, localhost as 127.0.0.1, names lower case.
std::string canonicalHost(const std::string &host);

}}

#endif
