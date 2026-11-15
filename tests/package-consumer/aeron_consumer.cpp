// Links the Aeron half of the installed package (client, client_aeron, runtime_aeron, runtime_embedded_driver,
// sbe_codecs) and runs it: an embedded media driver in <parent-dir>, an Aeron client with the fatal monitor, a
// ClusterClient over the Aeron transport with a GATEWAY core session registrar, a connect cycle against an endpoint
// where no cluster listens (it must stay non-blocking and never reach CONNECTED), the conductor watch, and an orderly
// close. Exit 0 and "aeron consumer OK" = pass.
//
// usage: aeron_consumer <parent-dir>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "cluster_tools/build_info.h"
#include "cluster_tools/client/ClusterClient.h"
#include "cluster_tools/client/CoreSessionRegistrar.h"
#include "cluster_tools/client/aeron/AeronTransport.h"
#include "cluster_tools/client/aeron/FatalErrors.h"
#include "cluster_tools/runtime/ConductorWatch.h"
#include "cluster_tools/runtime/EmbeddedDriver.h"

namespace cl = cluster_tools::client;
namespace at = cluster_tools::client::aeron_transport;
namespace rt = cluster_tools::runtime;

namespace
{

int failures = 0;

void expect(bool ok, const char *what)
{
    if (!ok)
    {
        std::fprintf(stderr, "aeron consumer: FAILED %s\n", what);
        failures++;
    }
}

}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: aeron_consumer <parent-dir>\n");
        return 64;
    }
    expect(cluster_tools::build_info::MONEY_SCHEMA_VERSION > 0, "build_info.h names the money schema version");

    rt::EmbeddedDriverOptions options;
    options.processName = "package-consumer";
    options.dirPrefix = "package-consumer-";
    options.instanceKey = "CONSUMER_INSTANCE_ID";
    const std::string dir = rt::EmbeddedDriver::dirFor(argv[1], 1, options);
    auto driver = std::make_unique<rt::EmbeddedDriver>(options,
        [](const std::string &why) { std::fprintf(stderr, "aeron consumer: driver terminated: %s\n", why.c_str()); });
    std::string error;
    if (!driver->start(dir, false, error))
    {
        std::fprintf(stderr, "aeron consumer: FAILED the embedded driver did not start: %s\n", error.c_str());
        return 1;
    }

    auto monitor = std::make_shared<at::AeronFatalMonitor>();
    at::AeronClientOptions clientOptions;
    clientOptions.aeronDir = dir;
    clientOptions.clientName = "package-consumer";
    clientOptions.driverTimeoutMs = 10'000;
    std::shared_ptr<aeron::Aeron> aeron = at::connectAeron(clientOptions, monitor);

    {
        at::AeronTransport transport(aeron);
        cl::ClientSettings settings;
        settings.ingressEndpoints = "0=127.0.0.1:1"; // nothing listens: the connect cycle must not block
        settings.ingressChannel = "aeron:udp?term-length=64k|mtu=8192";
        settings.responseChannel = "aeron:udp?endpoint=127.0.0.1:0|term-length=64k|mtu=8192";
        settings.credentials = std::vector<std::uint8_t>(cl::CoreCredentials::ENCODED_LENGTH, 1);
        settings.maxConnectAttempts = 1;
        settings.messageTimeoutNs = 200'000'000LL;
        expect(cl::validate(settings).empty(), "the client settings are valid");

        using Client = cl::ClusterClient<at::AeronTransport>;
        Client client(transport, settings);
        cl::RegistrarSettings registrarSettings;
        registrarSettings.sessionClass = cl::SessionClass::GATEWAY;
        registrarSettings.credentialClass = cl::SessionClass::GATEWAY;
        registrarSettings.channels = cl::core_channel::ACKS | cl::core_channel::CONTROL;
        registrarSettings.identity = 1;
        cl::CoreSessionRegistrar<Client> registrar(client, registrarSettings);
        expect(registrar.validateSettings().empty(), "a GATEWAY registrar is valid");

        expect(client.connect(), "connect() starts a cycle");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline && cl::ClientState::CONNECTING == client.state())
        {
            client.doWork();
            registrar.doWork();
            client.poll([&](const cl::EgressMessage &m) { registrar.onEgress(m); }, 10);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expect(cl::ClientState::FAILED == client.state(), "with no cluster the bounded connect cycle ends FAILED");
        expect(!registrar.isRegistered(), "nothing is registered without a session");
        expect(client.counters().connectAttempts >= 1, "at least one connect attempt was made");

        rt::ConductorWatch watch(aeron, 10'000);
        std::string why;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        expect(watch.check(why), "the conductor watch reads a live client conductor");
    }

    monitor->beginDeliberateClose();
    aeron.reset();
    expect(driver->close(5'000), "the embedded driver closes inside its bound");
    expect(!monitor->isFatal(), "no fatal Aeron error");

    if (0 != failures)
    {
        return 1;
    }
    std::printf("aeron consumer OK (money schema v%d)\n", cluster_tools::build_info::MONEY_SCHEMA_VERSION);
    return 0;
}
