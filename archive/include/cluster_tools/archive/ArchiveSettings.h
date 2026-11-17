#ifndef CLUSTER_TOOLS_ARCHIVE_ARCHIVE_SETTINGS_H
#define CLUSTER_TOOLS_ARCHIVE_ARCHIVE_SETTINGS_H

// Strict settings of an archive reader (the archive keys plus <prefix>JOURNAL_MTU). parse() returns every error
// it finds and never a default: a missing or invalid key refuses the start (exit 78, row C1). Pure. The key
// names take a prefix: the bridge keeps its BRIDGE_ names (the default), another owner names its own (for
// example HISTORY_PROJECTOR_JOURNAL_ARCHIVES, ..._JOURNAL_MTU).

#include "cluster_tools/archive/FramePeek.h"

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
    std::vector<ArchiveEndpoint> archives; // <prefix><archives key> (BRIDGE_ME_JOURNAL_ARCHIVES), in failover order
    std::int32_t controlStreamId = 0;      // <prefix>JOURNAL_CONTROL_STREAM
    std::int32_t journalStreamId = 0;      // <prefix>JOURNAL_STREAM
    std::string journalChannel;            // <prefix>JOURNAL_CHANNEL
    std::int32_t replayStreamId = 0;       // <prefix>REPLAY_STREAM
    std::string localHost;                 // <prefix>LOCAL_HOST
    std::int32_t journalMtu = 0;           // <prefix>JOURNAL_MTU
    std::string driverDir;                 // <prefix>DRIVER_DIR (scope EMBEDDED only)
};

// The key names of one owner. The default is the bridge's: BRIDGE_ME_JOURNAL_ARCHIVES and BRIDGE_<key>, the MTU
// bound by the ME journal's largest frame, the removed BRIDGE_HALT_ON_GAP refused.
struct ArchiveSettingsKeys
{
    std::string prefix = "BRIDGE_";
    std::string archives = "ME_JOURNAL_ARCHIVES"; // the archive list's key after the prefix
    std::string role = "the bridge";              // how errors name the process
    JournalProfile profile = ME_JOURNAL;          // <prefix>JOURNAL_MTU must carry its largest frame
    bool refuseHaltOnGap = true;

    // <prefix>JOURNAL_ARCHIVES, <prefix>JOURNAL_MTU, ... for a journal of `profile`.
    static ArchiveSettingsKeys withPrefix(const std::string &prefix, const JournalProfile &profile,
        const std::string &role = "the process");
};

struct ArchiveSettingsResult
{
    std::optional<ArchiveSettings> settings;
    std::vector<std::string> errors;

    bool ok() const noexcept { return settings.has_value(); }
};

// The bridge's media driver is configurable. BRIDGE_EMBEDDED is the bridge
// process with its own in-process driver (dev and demo; it creates <BRIDGE_DRIVER_DIR>/ct-bridge-<id>);
// BRIDGE_EXTERNAL is the bridge on the host aeronmd at AERON_DIR (the prod default), where BRIDGE_DRIVER_DIR is
// refused if present, so a unit cannot carry a key that means nothing in its mode.
enum class SettingsScope : std::uint8_t
{
    BRIDGE_EMBEDDED, // <prefix>DRIVER_DIR is required (the parent of the per-instance driver dir)
    BRIDGE_EXTERNAL, // <prefix>DRIVER_DIR is refused if present (the driver is the host aeronmd)
    TOOL,            // ct-journal-tail: an external driver, <prefix>DRIVER_DIR not read
    EMBEDDED = BRIDGE_EMBEDDED,
    EXTERNAL = BRIDGE_EXTERNAL
};

using SettingLookup = std::function<std::optional<std::string>(const std::string &key)>;

// Every key named in 4.1, plus the refused BRIDGE_HALT_ON_GAP: the bridge's keys.
ArchiveSettingsResult parseArchiveSettings(const SettingLookup &lookup, SettingsScope scope);
// The same rules under another owner's key names.
ArchiveSettingsResult parseArchiveSettings(const SettingLookup &lookup, SettingsScope scope, const ArchiveSettingsKeys &keys);

// localhost, every 127/8 form inet_aton accepts, ::1 and ::ffff:127.x.
bool isLoopbackHost(const std::string &host) noexcept;
// The comparison form of a host: canonical IPv4/IPv6 text, localhost as 127.0.0.1, names lower case.
std::string canonicalHost(const std::string &host);

}}

#endif
