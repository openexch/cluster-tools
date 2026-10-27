// switch-behavior — operator CLI for the behaviour-versioning switch flow.
//
// Working today:
//   probe        connect to a cluster, print the session info and the first N egress
//                frames decoded at header level, exit 0.
//   send-config  send the logged EngineConfig command (match wire v12, template 8)
//                from a JSON file: parse -> normalize -> validate against the
//                engine's own rules -> encode -> offer through the cluster session.
//
// TODO(switch-behavior): the SwitchBehavior{targetVersion} subcommand lands here once
// its wire schema clears design review. The decision table it must respect already
// lives in domain/include/cluster_tools/domain/SwitchDecision.h.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "cluster_tools/adapters/aeron/AeronClusterSession.h"
#include "cluster_tools/adapters/json/EngineConfigJson.h"
#include "cluster_tools/adapters/sbe/EngineConfigCodec.h"
#include "cluster_tools/domain/EngineConfig.h"

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
// four little-endian uint16s), decoded by hand: header-level egress reporting must not
// depend on any engine's generated codecs — the probe's whole point, and enough for
// send-config too (EngineConfig has no per-command ack egress to decode).
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

// Connection flags shared by every subcommand (flag wins over env).
struct ConnectionOptions
{
    std::string ingress;
    std::string aeronDir;
    std::string responseChannel;
};

ConnectionOptions connectionDefaultsFromEnv()
{
    ConnectionOptions options;
    options.ingress = envOr("INGRESS_ENDPOINTS", "");
    options.aeronDir = envOr("AERON_DIR", "");
    const std::string selfIp = envOr("SELF_IP", "");
    if (!selfIp.empty())
    {
        options.responseChannel = "aeron:udp?endpoint=" + selfIp + ":19999";
    }
    return options;
}

struct ProbeOptions
{
    ConnectionOptions connection;
    int frames = 5;
    std::int64_t timeoutMs = 20 * 1000;
};

struct SendConfigOptions
{
    ConnectionOptions connection;
    std::string file;
    std::int64_t pollMs = 3 * 1000;
};

void printUsage()
{
    std::printf(
        "usage: switch-behavior <subcommand> [options]\n"
        "\n"
        "subcommands:\n"
        "  probe        connect to an Aeron Cluster, print the session info and the first\n"
        "               N egress frames decoded at header level, then exit\n"
        "  send-config  send the logged EngineConfig command (match wire v12, template 8)\n"
        "               read from a JSON file through cluster ingress\n"
        "\n"
        "common options (flag wins over env):\n"
        "  --ingress SPEC            cluster ingress endpoints \"id=host:port,...\"\n"
        "                            (env: INGRESS_ENDPOINTS)\n"
        "  --response-channel CHAN   egress channel the cluster answers on, e.g.\n"
        "                            aeron:udp?endpoint=172.28.0.20:19999 — must be an\n"
        "                            address routable FROM the cluster, never 0.0.0.0\n"
        "                            or localhost (env: SELF_IP builds\n"
        "                            aeron:udp?endpoint=$SELF_IP:19999)\n"
        "  --aeron-dir DIR           media driver directory (env: AERON_DIR;\n"
        "                            default: the aeron default for this host)\n"
        "\n"
        "probe options:\n"
        "  --frames N                egress frames to print before exiting (default 5)\n"
        "  --timeout-ms MS           egress wait budget after connect (default 20000)\n"
        "\n"
        "send-config options:\n"
        "  --file PATH               engine-config JSON document (required):\n"
        "                            {\"configVersion\":1, \"impl\":\"array\"|\"direct\",\n"
        "                             \"bookCapacity\":131072, \"maxMatchesPerOrder\":10000,\n"
        "                             \"maxOrdersPerLevel\":0, \"behaviorVersion\":1,\n"
        "                             \"commandLedgerCapacity\":100000,\n"
        "                             \"genesisId\":<random non-zero int64>,\n"
        "                             \"maxWireVersion\":12, \"markets\":[{\"marketId\":1,\n"
        "                             \"symbol\":\"BTC-USD\", \"minPrice\":..., \"maxPrice\":...,\n"
        "                             \"tickSize\":...}]}\n"
        "                            every key is required; prices are fixed-point int64\n"
        "                            (8 decimals); markets are normalized (sorted by\n"
        "                            marketId) before validation and encoding\n"
        "  --poll-ms MS              how long to watch egress after the offer (default\n"
        "                            3000) — reporting only, see the note below\n"
        "\n"
        "send-config semantics: the config is validated with the ENGINE's own rules\n"
        "before anything touches the network (plus tool-side rules: configVersion must be\n"
        "a positive uint32; behaviorVersion >= 1; commandLedgerCapacity in [1, uint32];\n"
        "genesisId non-zero, supplied by the operator, never generated here;\n"
        "maxWireVersion in [11, 12]). EngineConfig has NO per-command ack on egress: the\n"
        "engine decides ACCEPT / duplicate no-op / REJECT deterministically server-side\n"
        "and logs it. After offering, the tool prints the offer position, briefly\n"
        "reports egress at header level, and points you at the node logs\n"
        "([ENGINE-CONFIG] lines) and the match_engine_config_{accepted,duplicates,\n"
        "rejects}_total counters for the verdict.\n"
        "\n"
        "exit codes: 0 = done (probe: session established; send-config: command offered\n"
        "— the server-side verdict lives in the node logs); 1 = connect/offer failed or\n"
        "the cluster closed the session; 2 = usage, unreadable file, or a config the\n"
        "engine would reject.\n");
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

bool parseConnectionArg(int argc, char **argv, int &i, ConnectionOptions &connection)
{
    return parseArg(argc, argv, i, "--ingress", connection.ingress) ||
        parseArg(argc, argv, i, "--response-channel", connection.responseChannel) ||
        parseArg(argc, argv, i, "--aeron-dir", connection.aeronDir);
}

// Refuse ambiguity rather than guess: both endpoints must be explicit.
bool checkConnectionOptions(const ConnectionOptions &connection)
{
    if (connection.ingress.empty())
    {
        std::fprintf(stderr, "switch-behavior: no ingress endpoints "
            "(--ingress or INGRESS_ENDPOINTS)\n");
        return false;
    }
    if (connection.responseChannel.empty())
    {
        std::fprintf(stderr, "switch-behavior: no response channel "
            "(--response-channel or SELF_IP; the cluster must be able to route back)\n");
        return false;
    }
    return true;
}

int runProbe(const ProbeOptions &options)
{
    AeronClusterSessionSettings settings;
    settings.aeronDir = options.connection.aeronDir;
    settings.responseChannel = options.connection.responseChannel;
    settings.clientInfo = "name=switch-behavior version=0.1";

    AeronClusterSession session(settings);

    std::printf("probe: connecting ingress=%s response=%s\n",
        options.connection.ingress.c_str(), options.connection.responseChannel.c_str());

    const auto connected = session.connect(options.connection.ingress);
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

int runSendConfig(const SendConfigOptions &options)
{
    // 1. Read the document. The app owns file I/O — the parser is pure text-in.
    std::ifstream file(options.file, std::ios::binary);
    if (!file)
    {
        std::fprintf(stderr, "send-config: cannot read '%s'\n", options.file.c_str());
        return 2;
    }
    std::ostringstream text;
    text << file.rdbuf();

    // 2. Parse (shape) then 3. normalize + validate (the engine's semantics): a config
    // the engine would deterministically REJECT never touches the network.
    const auto parsed = cluster_tools::json_adapter::parseEngineConfig(text.str());
    if (!parsed.ok())
    {
        std::fprintf(stderr, "send-config: %s: %s\n", options.file.c_str(), parsed.error().c_str());
        return 2;
    }
    const cluster_tools::domain::EngineConfig config =
        cluster_tools::domain::normalized(parsed.value());
    const std::string invalid = cluster_tools::domain::invalidReason(config);
    if (!invalid.empty())
    {
        std::fprintf(stderr, "send-config: %s: the engine would reject this config: %s\n",
            options.file.c_str(), invalid.c_str());
        return 2;
    }

    std::printf("send-config: configVersion=%lld impl=%s bookCapacity=%lld "
        "maxMatchesPerOrder=%lld maxOrdersPerLevel=%lld behaviorVersion=%lld "
        "commandLedgerCapacity=%lld genesisId=%lld maxWireVersion=%lld markets=%zu\n",
        static_cast<long long>(config.configVersion),
        cluster_tools::domain::implName(config.impl),
        static_cast<long long>(config.bookCapacity),
        static_cast<long long>(config.maxMatchesPerOrder),
        static_cast<long long>(config.maxOrdersPerLevel),
        static_cast<long long>(config.behaviorVersion),
        static_cast<long long>(config.commandLedgerCapacity),
        static_cast<long long>(config.genesisId),
        static_cast<long long>(config.maxWireVersion),
        config.markets.size());
    for (const auto &market : config.markets)
    {
        std::printf("send-config:   marketId=%d symbol=%s minPrice=%lld maxPrice=%lld tickSize=%lld\n",
            market.marketId, market.symbol.c_str(),
            static_cast<long long>(market.minPrice),
            static_cast<long long>(market.maxPrice),
            static_cast<long long>(market.tickSize));
    }

    // 4. Encode the normalized config as match-schema bytes.
    const auto encoded = cluster_tools::sbe_adapter::encodeEngineConfig(config);
    if (!encoded.ok())
    {
        std::fprintf(stderr, "send-config: %s\n", encoded.error().c_str());
        return 2;
    }

    // 5. Connect and offer; the session adapter adds the cluster framing
    // (SessionMessageHeader), exactly like the PoC's CreateOrder.
    AeronClusterSessionSettings settings;
    settings.aeronDir = options.connection.aeronDir;
    settings.responseChannel = options.connection.responseChannel;
    settings.clientInfo = "name=switch-behavior version=0.1";

    AeronClusterSession session(settings);

    std::printf("send-config: connecting ingress=%s response=%s\n",
        options.connection.ingress.c_str(), options.connection.responseChannel.c_str());

    const auto connected = session.connect(options.connection.ingress);
    if (!connected.ok())
    {
        std::fprintf(stderr, "send-config: FAIL %s\n", connected.error().c_str());
        return 1;
    }
    std::printf("send-config: CONNECTED sessionId=%lld leaderMemberId=%d leadershipTermId=%lld\n",
        static_cast<long long>(connected.value().sessionId),
        connected.value().leaderMemberId,
        static_cast<long long>(connected.value().leadershipTermId));

    const auto offered = session.offer(encoded.value().data(), encoded.value().size());
    if (!offered.ok())
    {
        std::fprintf(stderr, "send-config: FAIL %s\n", offered.error().c_str());
        return 1;
    }
    const auto wire = cluster_tools::sbe_adapter::engineConfigWireInfo();
    std::printf("send-config: OFFERED EngineConfig schemaId=%u version=%u templateId=%u "
        "payloadLength=%zu position=%lld\n",
        wire.schemaId, wire.schemaVersion, wire.templateId,
        encoded.value().size(), static_cast<long long>(offered.value()));

    // 6. Watch egress briefly, header-level only — reporting, not an ack wait (there
    // is no EngineConfig ack egress to wait for; see the note printed below).
    constexpr int PRINT_LIMIT = 10;
    int framesSeen = 0;
    const auto onEgress = [&](const EgressMessage &message)
    {
        framesSeen++;
        if (framesSeen > PRINT_LIMIT)
        {
            return;
        }
        SbeMessageHeader header{};
        if (decodeSbeMessageHeader(message.payload, message.length, header))
        {
            std::printf(
                "send-config: EGRESS[%d] schemaId=%u templateId=%u version=%u "
                "payloadLength=%zu\n",
                framesSeen, header.schemaId, header.templateId, header.version,
                message.length);
        }
        else
        {
            std::printf("send-config: EGRESS[%d] short frame payloadLength=%zu (no SBE header)\n",
                framesSeen, message.length);
        }
    };

    const std::int64_t deadlineMs = nowMs() + options.pollMs;
    while (nowMs() < deadlineMs)
    {
        const auto polled = session.poll(onEgress, 10);
        if (!polled.ok())
        {
            std::fprintf(stderr, "send-config: FAIL %s\n", polled.error().c_str());
            return 1;
        }
        if (0 == polled.value())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    if (framesSeen > PRINT_LIMIT)
    {
        std::printf("send-config: EGRESS ... %d more frame(s) not printed\n",
            framesSeen - PRINT_LIMIT);
    }

    std::printf(
        "send-config: DONE offered at position=%lld; egressFrames=%d in %lldms\n"
        "send-config: NOTE EngineConfig has no per-command ack on egress. The engine\n"
        "decided ACCEPT (fresh/adopted), duplicate no-op, or REJECT deterministically\n"
        "server-side: check the node logs for [ENGINE-CONFIG] lines and the\n"
        "match_engine_config_{accepted,duplicates,rejects}_total counters.\n",
        static_cast<long long>(offered.value()), framesSeen,
        static_cast<long long>(options.pollMs));
    return 0;
}

int mainProbe(int argc, char **argv)
{
    ProbeOptions options;
    options.connection = connectionDefaultsFromEnv();

    for (int i = 2; i < argc; i++)
    {
        std::string value;
        if (parseConnectionArg(argc, argv, i, options.connection))
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

    if (!checkConnectionOptions(options.connection))
    {
        return 2;
    }
    return runProbe(options);
}

int mainSendConfig(int argc, char **argv)
{
    SendConfigOptions options;
    options.connection = connectionDefaultsFromEnv();

    for (int i = 2; i < argc; i++)
    {
        std::string value;
        if (parseConnectionArg(argc, argv, i, options.connection) ||
            parseArg(argc, argv, i, "--file", options.file))
        {
            continue;
        }
        if (parseArg(argc, argv, i, "--poll-ms", value))
        {
            options.pollMs = std::atoll(value.c_str());
            if (options.pollMs < 1)
            {
                std::fprintf(stderr, "switch-behavior: --poll-ms wants a positive integer\n");
                return 2;
            }
            continue;
        }
        std::fprintf(stderr, "switch-behavior: unknown option '%s'\n\n", argv[i]);
        printUsage();
        return 2;
    }

    if (options.file.empty())
    {
        std::fprintf(stderr, "switch-behavior: send-config needs --file <engine-config.json>\n");
        return 2;
    }
    if (!checkConnectionOptions(options.connection))
    {
        return 2;
    }
    return runSendConfig(options);
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
    if ("probe" == subcommand)
    {
        return mainProbe(argc, argv);
    }
    if ("send-config" == subcommand)
    {
        return mainSendConfig(argc, argv);
    }

    std::fprintf(stderr, "switch-behavior: unknown subcommand '%s'\n\n", subcommand.c_str());
    printUsage();
    return 2;
}
