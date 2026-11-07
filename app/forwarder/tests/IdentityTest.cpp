// RC2 S10' 8.3 MainTest.identityIsTheConfiguredIdOnBothClusters (8.2 rule 3, review N-8): the forwarder's identity is
// FWD_INSTANCE_ID on BOTH cluster sessions, across restarts. main builds the agent and the two clients exactly as below
// (parseForwarderConfig -> forwarderIdentity / coreClientSettings / meClientSettings); three "starts" from one
// environment each run the agent over two FakeTransports to a registered core session and a bound ME session.
// On the core the identity is RegisterSession.identity (the field the core keeps per session) and the connect
// request's clientInfo; on the ME, whose session has no identity field (no wire change in S10'), it is the connect
// request's clientInfo ("name=ct-forwarder-me identity=<id> cluster-tools-client"). RED IDENTITY_RANDOM: a random
// identity per start (the S12 shape).
#include "doctest/doctest.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "AgentRig.h"
#include "ForwarderConfig.h"

#include "io_aeron_cluster_codecs/MessageHeader.h"
#include "io_aeron_cluster_codecs/SessionConnectRequest.h"

using namespace cluster_tools::app::forwarder;
using namespace cluster_tools::app::forwarder::test;

namespace {

const std::string SECRET(64, '6');

std::map<std::string, std::string> env()
{
    return {
        { "FWD_CORE_INGRESS", CORE_ENDPOINTS },
        { "FWD_CORE_RESPONSE_CHANNEL", "aeron:udp?endpoint=127.0.0.1:0" },
        { "AE_AUTH_CREDENTIAL", "6:FORWARDER:" + SECRET },
        { "FWD_ME_INGRESS", ME_ENDPOINTS },
        { "FWD_ME_RESPONSE_CHANNEL", "aeron:udp?endpoint=127.0.0.1:0" },
        { "ME_AUTH_CREDENTIAL", "7:FORWARDER:" + SECRET },
        { "FWD_INGRESS_CHANNEL", "aeron:udp?term-length=4m|mtu=8192" },
        { "FWD_DRIVER_MODE", "embedded" },
        { "FWD_DRIVER_DIR", "/dev/shm/fwd-identity-test" },
        { "FWD_DRIVER_TIMEOUT_MS", "5000" },
        { "FWD_INSTANCE_ID", "4242" },
        { "FWD_HTTP_BIND", "127.0.0.1:9600" },
    };
}

// The clientInfo of every SessionConnectRequest offered on `t`.
std::vector<std::string> connectClientInfos(const FakeTransport &t)
{
    std::vector<std::string> out;
    for (const auto &p : t.publications)
    {
        for (const Bytes &frame : p.offered)
        {
            if (frame.size() < cc::MessageHeader::encodedLength())
            {
                continue;
            }
            cc::MessageHeader header(const_cast<char *>(reinterpret_cast<const char *>(frame.data())), 0, frame.size(), 0);
            if (header.templateId() != cc::SessionConnectRequest::sbeTemplateId())
            {
                continue;
            }
            cc::SessionConnectRequest request(const_cast<char *>(reinterpret_cast<const char *>(frame.data())),
                cc::MessageHeader::encodedLength(), frame.size(), header.blockLength(), header.version());
            (void)request.getResponseChannelAsString(); // var fields are read in order
            (void)request.getEncodedCredentialsAsString();
            out.push_back(request.getClientInfoAsString());
        }
    }
    return out;
}

// One start: the config main parses, the clients and the agent main builds, run to REGISTERED + BOUND.
struct Start
{
    ForwarderConfig config;
    FakeTransport coreT;
    FakeTransport meT;
    std::unique_ptr<Client> core;
    std::unique_ptr<Client> me;
    LogRing log;
    std::unique_ptr<Agent> agent;
    std::int64_t coreSession = 501;
    std::int64_t meSession = 601;
    std::set<std::int64_t> answered;

    explicit Start(const std::map<std::string, std::string> &e)
    {
        cluster_tools::app::common::DirProbe anyDir; // FWD_DRIVER_DIR is only named here, never created
        anyDir.isDirectory = [](const std::string &) { return true; };
        anyDir.isWritable = [](const std::string &) { return true; };
        const ConfigResult r = parseForwarderConfig(
            [&e](const char *name) -> const char * {
                const auto it = e.find(name);
                return it == e.end() ? nullptr : it->second.c_str();
            },
            anyDir);
        REQUIRE(r.ok());
        config = r.config;
        meT.correlation = 50'000;
        core = std::make_unique<Client>(coreT, coreClientSettings(config));
        me = std::make_unique<Client>(meT, meClientSettings(config));
        AgentSettings s;
        s.identity = forwarderIdentity(config); // main: the same call (8.2 rule 3)
        s.version = "test";
        agent = std::make_unique<Agent>(*core, *me, s, &log);
    }

    void cycles(int n)
    {
        for (int i = 0; i < n && !agent->stopped(); i++)
        {
            coreT.now += MS;
            meT.now += MS;
            const std::int64_t c = coreT.lastConnectCorrelation();
            if (c > 0 && answered.insert(c).second)
            {
                coreT.pushSessionEvent(c, ++coreSession, 3, 0, cc::EventCode::OK, "", CORE_IMAGE);
            }
            const std::int64_t m = meT.lastConnectCorrelation();
            if (m > 0 && answered.insert(m).second)
            {
                meT.pushSessionEvent(m, ++meSession, 7, 0, cc::EventCode::OK, "", ME_IMAGE);
            }
            agent->doWork();
        }
    }

    // RegisterSession identities offered on the core
    std::vector<std::int64_t> registeredIdentities()
    {
        cycles(50);
        REQUIRE(core->isConnected());
        REQUIRE(me->isConnected());
        meT.pushAppMessage(meSession, heartbeat(EPOCH), ME_IMAGE, 7);
        cycles(10);
        REQUIRE(agent->meLink() == MeLink::BOUND);
        std::vector<std::int64_t> out;
        for (const Offered &o : appOffers(coreT))
        {
            if (80 == o.templateId)
            {
                out.push_back(decode80(o.payload).identity);
            }
        }
        return out;
    }
};

}

TEST_CASE("MainTest.identityIsTheConfiguredIdOnBothClusters")
{
    const std::string want = "identity=4242 ";
    for (int restart = 0; restart < 3; ++restart)
    {
        CAPTURE(restart);
        Start start(env());
        const std::vector<std::int64_t> identities = start.registeredIdentities();
        REQUIRE(!identities.empty());
        for (const std::int64_t id : identities)
        {
            CHECK(id == 4242); // IDENTITY_RANDOM RED: a random identity per start
        }
        const std::vector<std::string> coreInfos = connectClientInfos(start.coreT);
        const std::vector<std::string> meInfos = connectClientInfos(start.meT);
        REQUIRE(!coreInfos.empty());
        REQUIRE(!meInfos.empty());
        for (const std::string &info : coreInfos)
        {
            CAPTURE(info);
            CHECK(info.find("name=ct-forwarder-core " + want) == 0);
        }
        for (const std::string &info : meInfos)
        {
            CAPTURE(info);
            CHECK(info.find("name=ct-forwarder-me " + want) == 0);
        }
        // /health and forwarder_info carry the same identity
        ForwarderSnapshot snapshot;
        start.agent->fillSnapshot(snapshot);
        CHECK(snapshot.identity == 4242);
    }
}
