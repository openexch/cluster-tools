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
    std::string driverDir;                 // BRIDGE_DRIVER_DIR (scope BRIDGE_EMBEDDED only)
};

struct ArchiveSettingsResult
{
    std::optional<ArchiveSettings> settings;
    std::vector<std::string> errors;

    bool ok() const noexcept { return settings.has_value(); }
};

// RC2 S9c W0 (2026-10-31, checklist C13): the bridge's media driver is configurable. BRIDGE_EMBEDDED is the bridge
// process with its own in-process driver (dev and demo; it creates <BRIDGE_DRIVER_DIR>/ct-bridge-<id>);
// BRIDGE_EXTERNAL is the bridge on the host aeronmd at AERON_DIR (the prod default), where BRIDGE_DRIVER_DIR is
// refused if present, so a unit cannot carry a key that means nothing in its mode.
enum class SettingsScope : std::uint8_t
{
    BRIDGE_EMBEDDED, // BRIDGE_DRIVER_DIR is required (the parent of the per-instance driver dir)
    BRIDGE_EXTERNAL, // BRIDGE_DRIVER_DIR is refused if present (the driver is the host aeronmd)
    TOOL             // ct-journal-tail: an external driver, BRIDGE_DRIVER_DIR not read
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
