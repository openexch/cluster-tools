// switch-behavior — operator CLI for the behaviour-versioning switch flow.
//
// Working today:
//   probe   connect to a cluster, print the session info and the first N egress
//           frames decoded at header level, exit 0.
//
// TODO(switch-behavior): the real subcommand — sending EngineConfig /
// SwitchBehavior{targetVersion} through ingress and awaiting the ack — lands here once
// its wire schema clears design review. The decision table it must respect already
// lives in domain/include/cluster_tools/domain/SwitchDecision.h; this shell exists so
// that landing it is a small diff, not a new tool.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <thread>

#include "cluster_tools/adapters/aeron/AeronClusterSession.h"

namespace
{

using cluster_tools::aeron_adapter::AeronClusterSession;
using cluster_tools::aeron_adapter::AeronClusterSessionSettings;
using cluster_tools::ports::EgressMessage;

std::int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string envOr(const char *name, const std::string &dflt)
{
    const char *value = ::getenv(name);
    return (nullptr != value && '\0' != *value) ? std::string(value) : dflt;
}

// The standard 8-byte SBE message header (blockLength, templateId, schemaId, version —
// four little-endian uint16s), decoded by hand: header-level probing must not depend
// on any engine's generated codecs, that is the whole point of the probe.
struct SbeMessageHeader
{
    std::uint16_t blockLength;
    std::uint16_t templateId;
    std::uint16_t schemaId;
    std::uint16_t version;
};

std::uint16_t readUint16Le(const std::uint8_t *bytes)
{
    return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
}

bool decodeSbeMessageHeader(const std::uint8_t *bytes, std::size_t length, SbeMessageHeader &header)
{
    if (length < 8)
    {
        return false;
    }
    header.blockLength = readUint16Le(bytes);
    header.templateId = readUint16Le(bytes + 2);
    header.schemaId = readUint16Le(bytes + 4);
    header.version = readUint16Le(bytes + 6);
    return true;
}

struct ProbeOptions
{
    std::string ingress;
    std::string aeronDir;
    std::string responseChannel;
    int frames = 5;
    std::int64_t timeoutMs = 20 * 1000;
};

void printUsage()
{
    std::printf(
        "usage: switch-behavior probe [options]\n"
        "\n"
        "Connects to an Aeron Cluster, prints the session info and the first N egress\n"
        "frames decoded at header level, then exits.\n"
        "\n"
        "options (flag wins over env):\n"
        "  --ingress SPEC            cluster ingress endpoints \"id=host:port,...\"\n"
        "                            (env: INGRESS_ENDPOINTS)\n"
        "  --response-channel CHAN   egress channel the cluster answers on, e.g.\n"
        "                            aeron:udp?endpoint=172.28.0.20:19999 — must be an\n"
        "                            address routable FROM the cluster, never 0.0.0.0\n"
        "                            or localhost (env: SELF_IP builds\n"
        "                            aeron:udp?endpoint=$SELF_IP:19999)\n"
        "  --aeron-dir DIR           media driver directory (env: AERON_DIR;\n"
        "                            default: the aeron default for this host)\n"
        "  --frames N                egress frames to print before exiting (default 5)\n"
        "  --timeout-ms MS           egress wait budget after connect (default 20000)\n"
        "\n"
        "exit codes: 0 = session established (frames are best effort within the\n"
        "timeout); 1 = connect failed or the cluster closed the session; 2 = usage.\n");
}

bool parseArg(int argc, char **argv, int &i, const char *name, std::string &out)
{
    const std::size_t nameLength = std::strlen(name);
    if (0 == std::strncmp(argv[i], name, nameLength) && '=' == argv[i][nameLength])
    {
        out = argv[i] + nameLength + 1;
        return true;
    }
    if (0 == std::strcmp(argv[i], name) && i + 1 < argc)
    {
        out = argv[++i];
        return true;
    }
    return false;
}

int runProbe(const ProbeOptions &options)
{
    AeronClusterSessionSettings settings;
    settings.aeronDir = options.aeronDir;
    settings.responseChannel = options.responseChannel;
    settings.clientInfo = "name=switch-behavior version=0.1";

    AeronClusterSession session(settings);

    std::printf("probe: connecting ingress=%s response=%s\n",
        options.ingress.c_str(), options.responseChannel.c_str());

    const auto connected = session.connect(options.ingress);
    if (!connected.ok())
    {
        std::fprintf(stderr, "probe: FAIL %s\n", connected.error().c_str());
        return 1;
    }
    std::printf("probe: CONNECTED sessionId=%lld leaderMemberId=%d leadershipTermId=%lld\n",
        static_cast<long long>(connected.value().sessionId),
        connected.value().leaderMemberId,
        static_cast<long long>(connected.value().leadershipTermId));

    int framesSeen = 0;
    const auto onEgress = [&](const EgressMessage &message)
    {
        if (framesSeen >= options.frames)
        {
            return; // drained in the same poll batch after the quota was met
        }
        framesSeen++;
        SbeMessageHeader header{};
        if (decodeSbeMessageHeader(message.payload, message.length, header))
        {
            std::printf(
                "probe: EGRESS[%d] schemaId=%u templateId=%u version=%u blockLength=%u "
                "payloadLength=%zu clusterSessionId=%lld\n",
                framesSeen, header.schemaId, header.templateId, header.version,
                header.blockLength, message.length,
                static_cast<long long>(message.clusterSessionId));
        }
        else
        {
            std::printf("probe: EGRESS[%d] short frame payloadLength=%zu (no SBE header)\n",
                framesSeen, message.length);
        }
    };

    const std::int64_t deadlineMs = nowMs() + options.timeoutMs;
    while (framesSeen < options.frames && nowMs() < deadlineMs)
    {
        const auto polled = session.poll(onEgress, 10);
        if (!polled.ok())
        {
            std::fprintf(stderr, "probe: FAIL %s\n", polled.error().c_str());
            return 1;
        }
        if (0 == polled.value())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    if (framesSeen < options.frames)
    {
        std::printf("probe: DONE framesSeen=%d of %d (egress quiet for %lldms — fine for an "
            "idle cluster)\n",
            framesSeen, options.frames, static_cast<long long>(options.timeoutMs));
    }
    else
    {
        std::printf("probe: DONE framesSeen=%d\n", framesSeen);
    }
    return 0; // RAII: the session destructor sends the close request and releases aeron
}

}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        printUsage();
        return 2;
    }
    const std::string subcommand = argv[1];
    if ("--help" == subcommand || "-h" == subcommand || "help" == subcommand)
    {
        printUsage();
        return 0;
    }
    if ("probe" != subcommand)
    {
        std::fprintf(stderr, "switch-behavior: unknown subcommand '%s'\n\n", subcommand.c_str());
        printUsage();
        return 2;
    }

    ProbeOptions options;
    options.ingress = envOr("INGRESS_ENDPOINTS", "");
    options.aeronDir = envOr("AERON_DIR", "");
    const std::string selfIp = envOr("SELF_IP", "");
    if (!selfIp.empty())
    {
        options.responseChannel = "aeron:udp?endpoint=" + selfIp + ":19999";
    }

    for (int i = 2; i < argc; i++)
    {
        std::string value;
        if (parseArg(argc, argv, i, "--ingress", options.ingress) ||
            parseArg(argc, argv, i, "--response-channel", options.responseChannel) ||
            parseArg(argc, argv, i, "--aeron-dir", options.aeronDir))
        {
            continue;
        }
        if (parseArg(argc, argv, i, "--frames", value))
        {
            options.frames = std::atoi(value.c_str());
            if (options.frames < 1)
            {
                std::fprintf(stderr, "switch-behavior: --frames wants a positive integer\n");
                return 2;
            }
            continue;
        }
        if (parseArg(argc, argv, i, "--timeout-ms", value))
        {
            options.timeoutMs = std::atoll(value.c_str());
            if (options.timeoutMs < 1)
            {
                std::fprintf(stderr, "switch-behavior: --timeout-ms wants a positive integer\n");
                return 2;
            }
            continue;
        }
        std::fprintf(stderr, "switch-behavior: unknown option '%s'\n\n", argv[i]);
        printUsage();
        return 2;
    }

    // Refuse ambiguity rather than guess: both of these must be explicit.
    if (options.ingress.empty())
    {
        std::fprintf(stderr, "switch-behavior: no ingress endpoints "
            "(--ingress or INGRESS_ENDPOINTS)\n");
        return 2;
    }
    if (options.responseChannel.empty())
    {
        std::fprintf(stderr, "switch-behavior: no response channel "
            "(--response-channel or SELF_IP; the cluster must be able to route back)\n");
        return 2;
    }

    return runProbe(options);
}
