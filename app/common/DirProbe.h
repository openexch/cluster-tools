#ifndef CLUSTER_TOOLS_APP_COMMON_DIR_PROBE_H
#define CLUSTER_TOOLS_APP_COMMON_DIR_PROBE_H

#include <sys/stat.h>
#include <unistd.h>

#include <functional>
#include <string>

// DirProbe - the file-system checks of the driver keys at the config parse ("one rule for both
// processes"): <PROCESS>_DRIVER_DIR must be an existing, writable directory (embedded) and AERON_DIR an
// existing directory (external), refused with every other config problem (78) before the http bind and any Aeron
// object. The config tests inject one; systemDirProbe() is stat(2) / access(2). The shape of ct-bridge's
// bridge::DirProbe (BridgeConfig.h), which can become an alias of this one.

namespace cluster_tools { namespace app { namespace common {

struct DirProbe
{
    std::function<bool(const std::string &)> isDirectory;
    std::function<bool(const std::string &)> isWritable;
};

inline DirProbe systemDirProbe()
{
    DirProbe p;
    p.isDirectory = [](const std::string &path) {
        struct stat st{};
        return 0 == ::stat(path.c_str(), &st) && S_ISDIR(st.st_mode);
    };
    p.isWritable = [](const std::string &path) { return 0 == ::access(path.c_str(), W_OK | X_OK); };
    return p;
}

}}}

#endif
