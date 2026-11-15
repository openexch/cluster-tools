#ifndef CLUSTER_TOOLS_APP_SESSION_STUB_STUB_COMMANDS_H
#define CLUSTER_TOOLS_APP_SESSION_STUB_STUB_COMMANDS_H

#include <cstdint>
#include <string>
#include <vector>

// The stub's stdin script: one command per line, executed in order on the duty thread. Blank lines and lines starting
// with '#' are ignored. A line that does not parse is reported (event=command_error) and skipped.
//
//   throttle off                    poll() every duty cycle
//   throttle <interval_ms> [<n>]    poll() at most once per interval, at most n fragments (default: --poll-fragments)
//   throttle pause                  no poll() at all; doWork() and the keepalives keep running
//   offer <hex>                     one tryOffer of these bytes (a money-schema message: header and body), result reported
//   status                          one status line now
//   sleep <ms>                      hold the script (not the duty cycle) for ms
//   await registered <timeout_ms>   hold the script until the registration is acked in the current session, or timeout
//   await closed <timeout_ms>       hold the script until a session closes after this line, or timeout
//   reconnect on|off                replace a closed or failed session (on) or stay closed (off)
//   close                           close the current session
//   quit                            close and exit 0

namespace cluster_tools { namespace app { namespace session_stub {

enum class CommandKind : std::uint8_t
{
    NONE,    // blank or comment
    INVALID, // a line that did not parse: reported in its place in the script (event=command_error), then skipped
    THROTTLE_OFF,
    THROTTLE,
    THROTTLE_PAUSE,
    OFFER,
    STATUS,
    SLEEP,
    AWAIT_REGISTERED,
    AWAIT_CLOSED,
    RECONNECT,
    CLOSE,
    QUIT,
};

struct Command
{
    CommandKind kind = CommandKind::NONE;
    std::int64_t valueMs = 0;   // THROTTLE interval, SLEEP, AWAIT_* timeout
    int fragments = 0;          // THROTTLE: 0 = the configured --poll-fragments
    bool flag = false;          // RECONNECT on
    std::vector<std::uint8_t> bytes; // OFFER
    std::string text;           // the line, for the report
    std::string error;          // INVALID: why it did not parse
};

struct CommandResult
{
    Command command;
    std::string error; // empty = parsed
    bool ok() const { return error.empty(); }
};

CommandResult parseCommand(const std::string &line);

}}}

#endif
