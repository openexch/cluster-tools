// Links cluster_tools::client_core and cluster_tools::runtime from the installed package and exercises one thing of
// each compiled part, so a missing object, header or include path fails here. Exit 0 and "pure consumer OK" = pass.
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#include "cluster_tools/client/ClientSettings.h"
#include "cluster_tools/client/Credentials.h"
#include "cluster_tools/client/IngressEndpoints.h"
#include "cluster_tools/client/Protocol.h"
#include "cluster_tools/runtime/DirProbe.h"
#include "cluster_tools/runtime/DriverTimeout.h"
#include "cluster_tools/runtime/HttpServer.h"
#include "cluster_tools/runtime/IdleStrategy.h"
#include "cluster_tools/runtime/LogRing.h"
#include "cluster_tools/runtime/StatusBoardT.h"
#include "cluster_tools/runtime/StorageStarvation.h"
#include "cluster_tools/runtime/Supervisor.h"

namespace cl = cluster_tools::client;
namespace rt = cluster_tools::runtime;

namespace
{

int failures = 0;

void expect(bool ok, const char *what)
{
    if (!ok)
    {
        std::fprintf(stderr, "pure consumer: FAILED %s\n", what);
        failures++;
    }
}

}

int main()
{
    // client_core
    const std::string spec = "7:gateway:" + std::string(64, 'a');
    const cl::CredentialsResult credentials = cl::parseCoreCredentials(spec);
    expect(credentials.ok(), "a GATEWAY credential spec parses");
    expect(cl::SessionClass::GATEWAY == credentials.credentials.sessionClass, "the class is GATEWAY");
    expect(cl::CoreCredentials::ENCODED_LENGTH == credentials.credentials.encode().size(), "the encoded credential is 38 bytes");
    expect(cl::parseIngressEndpoints("0=10.0.0.1:9302,1=10.0.0.2:9302").ok(), "ingress endpoints parse");
    cl::ClientSettings settings;
    expect(!cl::validate(settings).empty(), "settings without endpoints are refused");
    expect(std::string("CONNECTED") == cl::nameOf(cl::ClientState::CONNECTED), "the protocol vocabulary links");

    // runtime
    std::string error;
    expect(rt::parseDriverTimeoutMs("X_DRIVER_TIMEOUT_MS", "10000", error).value_or(0) == 10'000, "the driver timeout parses");
    expect(!rt::parseDriverTimeoutMs("X_DRIVER_TIMEOUT_MS", "10", error).has_value(), "a driver timeout below the floor is refused");
    expect(rt::isStorageSpaceError("insufficient usable storage for new log of length=12582912"), "the storage rule links");
    expect(rt::statusText(503) == "Service Unavailable", "HttpServer's status text links");
    rt::IdleStrategy idle(rt::BRIDGE_IDLE);
    idle.idle(1);
    rt::LogRing ring;
    expect(ring.push(rt::LogLevel::INFO, 1, "consumer %d", 1), "the log ring accepts a record");
    int devNull = ::open("/dev/null", O_WRONLY);
    rt::NonBlockingLogWriter writer(devNull);
    expect(1 == ring.drain(writer), "the supervisor's log writer links and drains one record");
    ::close(devNull);
    expect(rt::systemDirProbe().isDirectory("/"), "the directory probe links");

    if (0 != failures)
    {
        return 1;
    }
    std::printf("pure consumer OK\n");
    return 0;
}
