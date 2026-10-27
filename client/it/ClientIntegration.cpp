// Real-cluster integration scenarios for the C++ cluster client (RC2 S11, F2 RED list).
// Driven by client/it/run-it.sh, which starts the media driver, a 3-node assets (core)
// cluster and a 1-node match (ME) cluster from jars built at the dependencies.lock commits,
// and does the killing. Each scenario prints PASS/FAIL lines and exits 0 only if all passed.
//
//   ct_client_it auth          <aeronDir> <aeIngress> <responseHost>
//   ct_client_it keepalive     <aeronDir> <aeIngress> <responseHost>
//   ct_client_it two-clusters  <aeronDir> <aeIngress> <meIngress> <responseHost>
//   ct_client_it leader-kill   <aeronDir> <aeIngress> <responseHost>   (prints READY leader=N,
//                              waits for the kill; then PHASE2, waits for all nodes to die)
//   ct_client_it fatal         <aeronDir>                              (exits 70 when the driver dies)
//
// The scenario loop is the owner's duty cycle: doWork + poll per client, then a 100 us pause
// (the owner's idle strategy, not the client's). Every client call is timed; the slowest is
// reported and must stay under MAX_CALL_US (the client never blocks).

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cluster_tools/client/ClusterClient.h"
#include "cluster_tools/client/CoreSessionRegistrar.h"
#include "cluster_tools/client/Credentials.h"
#include "cluster_tools/client/aeron/AeronTransport.h"
#include "cluster_tools/client/aeron/FatalErrors.h"

#include "com_openexchange_assets_infrastructure_generated/AdmissionState.h"
#include "com_openexchange_assets_infrastructure_generated/MessageHeader.h"
#include "com_openexchange_assets_infrastructure_generated/QueryFeedPosition.h"
#include "com_match_infrastructure_generated/ClusterHeartbeat.h"
#include "com_match_infrastructure_generated/MessageHeader.h"

using namespace cluster_tools::client;
using cluster_tools::client::aeron_transport::AeronClientOptions;
using cluster_tools::client::aeron_transport::AeronFatalMonitor;
using cluster_tools::client::aeron_transport::AeronTransport;
using cluster_tools::client::aeron_transport::connectAeron;
namespace money = com::openexchange::assets::infrastructure::generated;
namespace me = com::match::infrastructure::generated;

namespace
{

// assets TestAuth.java keys (the run script installs the same table on the core).
// The ME authenticates every session since match S6 (ME_AUTH_KEYS in run-it.sh); the market-data
// class presents this key (id 5, class GATEWAY, 32 bytes of 0x55).
const std::string ME_GATEWAY_KEY = "5:GATEWAY:" + std::string(64, '5');
const std::string FORWARDER_KEY = "6:FORWARDER:" + std::string(64, '6');
const std::string WRONG_CLASS_KEY = "6:BRIDGE:" + std::string(64, '6');
constexpr std::int64_t MAX_CALL_US = 5'000;

int g_failures = 0;
std::int64_t g_slowestCallNs = 0;
std::string g_slowestCall;

void check(bool ok, const std::string &what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok)
    {
        g_failures++;
    }
}

std::int64_t nowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

template<typename F>
auto timed(const char *name, F &&f)
{
    const std::int64_t start = nowNs();
    struct Done
    {
        std::int64_t start;
        const char *name;
        ~Done()
        {
            const std::int64_t took = nowNs() - start;
            if (took > g_slowestCallNs)
            {
                g_slowestCallNs = took;
                g_slowestCall = name;
            }
        }
    } done{ start, name };
    return f();
}

struct Observer
{
    std::vector<NewLeaderInfo> newLeaders;
    std::vector<SessionEventInfo> events;
    void onNewLeader(const NewLeaderInfo &info)
    {
        std::printf("  new leader: term %lld -> %lld leader=%d offersInEndedTerm=%llu afterLastEgress=%llu\n",
            static_cast<long long>(info.previousLeadershipTermId), static_cast<long long>(info.leadershipTermId),
            info.leaderMemberId, static_cast<unsigned long long>(info.offersInEndedTerm),
            static_cast<unsigned long long>(info.offersAfterLastEgress));
        newLeaders.push_back(info);
    }
    void onSessionEvent(const SessionEventInfo &info) { events.push_back(info); }
    void onStateChange(ClientState from, ClientState to)
    {
        std::printf("  state %s -> %s\n", nameOf(from), nameOf(to));
        std::fflush(stdout);
    }
};

using Client = ClusterClient<AeronTransport, Observer>;
using Registrar = CoreSessionRegistrar<Client>;

ClientSettings coreSettings(const std::string &ingress, const std::string &host, int port, const std::string &key)
{
    ClientSettings s;
    s.ingressEndpoints = ingress;
    s.ingressChannel = "aeron:udp?term-length=4m|mtu=8192";
    s.responseChannel = "aeron:udp?endpoint=" + host + ":" + std::to_string(port);
    s.clientName = "ct-client-it";
    if (key.empty())
    {
        s.allowEmptyCredentials = true;
    }
    else
    {
        const CredentialsResult c = parseCoreCredentials(key);
        if (!c.ok())
        {
            std::fprintf(stderr, "bad key: %s\n", c.error.c_str());
            std::exit(2);
        }
        s.credentials = c.credentials.encode();
    }
    s.maxConnectAttempts = 20;
    return s;
}

RegistrarSettings forwarderRegistration()
{
    RegistrarSettings r;
    r.sessionClass = SessionClass::FORWARDER;
    r.credentialClass = parseCoreCredentials(FORWARDER_KEY).credentials.sessionClass;
    r.channels = core_channel::DISPATCH | core_channel::CONTROL;
    r.identity = 0x5110'0001;
    return r;
}

struct Session
{
    Session(std::shared_ptr<aeron::Aeron> aeron, ClientSettings settings, bool withRegistrar) :
        transport(std::move(aeron)), client(transport, std::move(settings), &observer)
    {
        if (withRegistrar)
        {
            registrar = std::make_unique<Registrar>(client, forwarderRegistration());
        }
    }

    void duty()
    {
        timed("doWork", [&] { return client.doWork(); });
        if (registrar)
        {
            timed("registrar.doWork", [&] { return registrar->doWork(); });
        }
        timed("poll", [&]
            {
                return client.poll([&](const EgressMessage &m)
                    {
                        egressMessages++;
                        if (registrar && registrar->onEgress(m))
                        {
                            return;
                        }
                        onApp(m);
                    }, 32);
            });
    }

    void onApp(const EgressMessage &m)
    {
        if (m.length < 8)
        {
            return;
        }
        std::uint16_t schemaId;
        std::uint16_t templateId;
        std::uint16_t blockLength;
        std::uint16_t version;
        std::memcpy(&blockLength, m.payload, 2);
        std::memcpy(&templateId, m.payload + 2, 2);
        std::memcpy(&schemaId, m.payload + 4, 2);
        std::memcpy(&version, m.payload + 6, 2);
        if (schemaId == money::MessageHeader::sbeSchemaId() && templateId == money::AdmissionState::sbeTemplateId())
        {
            admissionStates++;
            // v7: gateReasons appended, block 23 -> 27. The block on the wire is the frame's own.
            admissionBlock = blockLength;
            admissionVersion = version;
        }
        if (schemaId == me::MessageHeader::sbeSchemaId())
        {
            meFrames++;
            if (21 == templateId)
            {
                meHeartbeats++;
                // v12: meEpoch appended, block 9 -> 17. Decode with the frame's OWN blockLength and
                // version through the generated codec (never a compiled-in length).
                heartbeatBlock = blockLength;
                heartbeatVersion = version;
                me::ClusterHeartbeat hb(const_cast<char *>(reinterpret_cast<const char *>(m.payload)),
                    me::MessageHeader::encodedLength(), m.length, blockLength, version);
                heartbeatTimestamp = hb.timestamp();
                heartbeatHasEpoch = hb.meEpochInActingVersion();
                if (heartbeatHasEpoch)
                {
                    heartbeatEpoch = hb.meEpoch();
                }
            }
        }
    }

    AeronTransport transport;
    Observer observer;
    Client client;
    std::unique_ptr<Registrar> registrar;
    std::uint64_t egressMessages = 0;
    std::uint64_t admissionStates = 0;
    std::uint64_t meFrames = 0;
    std::uint64_t meHeartbeats = 0;
    std::uint16_t admissionBlock = 0;
    std::uint16_t admissionVersion = 0;
    std::uint16_t heartbeatBlock = 0;
    std::uint16_t heartbeatVersion = 0;
    std::int64_t heartbeatTimestamp = 0;
    bool heartbeatHasEpoch = false;
    std::int64_t heartbeatEpoch = 0;
};

bool runUntil(std::vector<Session *> sessions, const std::function<bool()> &done, std::int64_t timeoutMs)
{
    const std::int64_t deadline = nowNs() + timeoutMs * 1'000'000;
    while (nowNs() < deadline)
    {
        for (Session *s : sessions)
        {
            s->duty();
        }
        if (done())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return done();
}

std::shared_ptr<aeron::Aeron> aeronFor(const std::string &dir, const std::shared_ptr<AeronFatalMonitor> &monitor)
{
    AeronClientOptions options;
    options.aeronDir = dir;
    options.clientName = "ct-client-it";
    return connectAeron(options, monitor);
}

int finish(AeronFatalMonitor &monitor)
{
    check(!monitor.isFatal(), "no fatal aeron error (" + monitor.fatalMessage() + ")");
    check(g_slowestCallNs < MAX_CALL_US * 1000, "slowest client call " + g_slowestCall + " took " +
        std::to_string(g_slowestCallNs / 1000) + " us (limit " + std::to_string(MAX_CALL_US) + " us): never blocks");
    std::printf("%s %d failure(s)\n", 0 == g_failures ? "SCENARIO PASS" : "SCENARIO FAIL", g_failures);
    monitor.beginDeliberateClose();
    return 0 == g_failures ? 0 : 1;
}

void printCounters(const char *label, const Client &c)
{
    const ClientCounters &k = c.counters();
    std::printf("  [%s] attempts=%llu failed=%llu redirects=%llu opened=%llu offers=%llu bp=%llu keepalives=%llu "
        "kaFail=%llu newLeaders=%llu imageLoss=%llu ingressDisc=%llu possiblyLost=%llu egress=%llu\n",
        label,
        (unsigned long long)k.connectAttempts, (unsigned long long)k.connectAttemptsFailed,
        (unsigned long long)k.redirects, (unsigned long long)k.sessionsOpened, (unsigned long long)k.offers,
        (unsigned long long)k.offersBackPressured, (unsigned long long)k.keepAlivesSent,
        (unsigned long long)k.keepAliveFailures, (unsigned long long)k.newLeaderEvents,
        (unsigned long long)k.egressImageLosses, (unsigned long long)k.ingressDisconnects,
        (unsigned long long)k.possiblyLostOffers, (unsigned long long)k.egressMessages);
}

// ---------------------------------------------------------------- scenarios

int scenarioAuth(const std::string &dir, const std::string &ingress, const std::string &host)
{
    auto monitor = std::make_shared<AeronFatalMonitor>();
    auto aeron = aeronFor(dir, monitor);
    {
        Session anonymous(aeron, coreSettings(ingress, host, 41001, ""), false);
        check(anonymous.client.connect(), "anonymous connect starts");
        runUntil({ &anonymous }, [&] { return anonymous.client.state() == ClientState::FAILED; }, 20'000);
        check(anonymous.client.connectError() == ConnectError::AUTHENTICATION_REJECTED,
            std::string("no credential -> AUTHENTICATION_REJECTED (got ") + nameOf(anonymous.client.connectError()) +
            ": " + anonymous.client.lastDetail() + ")");
    }
    {
        Session wrong(aeron, coreSettings(ingress, host, 41002, WRONG_CLASS_KEY), false);
        wrong.client.connect();
        runUntil({ &wrong }, [&] { return wrong.client.state() == ClientState::FAILED; }, 20'000);
        check(wrong.client.connectError() == ConnectError::AUTHENTICATION_REJECTED,
            std::string("FORWARDER key presented as BRIDGE -> AUTHENTICATION_REJECTED (got ") +
            nameOf(wrong.client.connectError()) + ")");
    }
    {
        Session ok(aeron, coreSettings(ingress, host, 41003, FORWARDER_KEY), true);
        ok.client.connect();
        runUntil({ &ok }, [&] { return ok.registrar->isRegistered() || ok.registrar->isRejected() ||
            ok.client.state() == ClientState::FAILED; }, 30'000);
        check(ok.client.isConnected(), std::string("FORWARDER credential -> CONNECTED (") + nameOf(ok.client.state()) +
            " " + ok.client.lastDetail() + ")");
        check(ok.registrar->isRegistered(), std::string("RegisterSession FORWARDER dispatch|control -> AdminAck ACCEPTED (") +
            nameOf(ok.registrar->state()) + ")");
        runUntil({ &ok }, [&] { return ok.admissionStates > 0; }, 5'000);
        check(ok.admissionStates > 0, "ORIGIN AdmissionState after registration");
        check(ok.admissionVersion == 7 && ok.admissionBlock == 27 &&
                ok.admissionBlock == money::AdmissionState::sbeBlockLength(),
            "AdmissionState arrives at money v7, block 27 (got v" + std::to_string(ok.admissionVersion) +
            " block " + std::to_string(ok.admissionBlock) + ")");
        printCounters("auth", ok.client);
        std::printf("  redirects on connect: %llu (leader %d)\n",
            static_cast<unsigned long long>(ok.client.counters().redirects), ok.client.leaderMemberId());
        const std::int32_t leader = ok.client.leaderMemberId();
        ok.client.close();

        // REDIRECT on the real cluster: list the followers first, so the first connected
        // publication is a follower's and the connect has to be redirected to the leader.
        const ParsedEndpoints parsed = parseIngressEndpoints(ingress);
        std::string followersFirst;
        std::string leaderEntry;
        for (const IngressMember &m : parsed.members)
        {
            const std::string entry = std::to_string(m.id) + "=" + m.endpoint;
            if (m.id == leader)
            {
                leaderEntry = entry;
            }
            else
            {
                followersFirst += (followersFirst.empty() ? "" : ",") + entry;
            }
        }
        followersFirst += "," + leaderEntry;
        Session redirected(aeron, coreSettings(followersFirst, host, 41004, FORWARDER_KEY), false);
        redirected.client.connect();
        runUntil({ &redirected }, [&] { return redirected.client.isConnected() ||
            redirected.client.state() == ClientState::FAILED; }, 30'000);
        check(redirected.client.isConnected() && redirected.client.leaderMemberId() == leader &&
            redirected.client.counters().redirects >= 1,
            "follower first in the list -> REDIRECT -> CONNECTED to leader " + std::to_string(leader) +
            " (redirects=" + std::to_string(redirected.client.counters().redirects) + ", endpoints " + followersFirst + ")");
        redirected.client.close();
    }
    return finish(*monitor);
}

int scenarioKeepAlive(const std::string &dir, const std::string &ingress, const std::string &host)
{
    auto monitor = std::make_shared<AeronFatalMonitor>();
    auto aeron = aeronFor(dir, monitor);
    ClientSettings silentSettings = coreSettings(ingress, host, 41011, FORWARDER_KEY);
    silentSettings.disableKeepAliveForTest = true;
    Session silent(aeron, silentSettings, false);
    Session alive(aeron, coreSettings(ingress, host, 41012, FORWARDER_KEY), false);
    silent.client.connect();
    alive.client.connect();
    runUntil({ &silent, &alive }, [&] { return silent.client.isConnected() && alive.client.isConnected(); }, 30'000);
    check(silent.client.isConnected() && alive.client.isConnected(), "both sessions connected");
    const std::int64_t start = nowNs();
    runUntil({ &silent, &alive }, [&] { return silent.client.state() == ClientState::CLOSED; }, 16'000);
    const double closedAfter = static_cast<double>(nowNs() - start) / 1e9;
    check(silent.client.state() == ClientState::CLOSED &&
        silent.client.closeReason() == CloseReason::SESSION_CLOSED_BY_CLUSTER,
        "silentClientIsClosedAfter10s: keepalive disabled -> closed by the cluster after " +
        std::to_string(closedAfter) + " s (" + silent.client.lastDetail() + ")");
    check(closedAfter > 8.0, "closed no earlier than the 10 s session timeout (minus connect time)");
    runUntil({ &alive }, [&] { return false; }, 2'000);
    check(alive.client.isConnected(), "the 1 s keepalive session stays open past the timeout (" +
        std::to_string(static_cast<double>(nowNs() - start) / 1e9) + " s)");
    check(alive.client.counters().keepAlivesSent >= 10, "keepalives sent: " +
        std::to_string(alive.client.counters().keepAlivesSent));
    printCounters("alive", alive.client);
    alive.client.close();
    return finish(*monitor);
}

int scenarioTwoClusters(const std::string &dir, const std::string &aeIngress, const std::string &meIngress,
    const std::string &host)
{
    auto monitor = std::make_shared<AeronFatalMonitor>();
    auto aeron = aeronFor(dir, monitor); // ONE client conductor for both sessions
    Session core(aeron, coreSettings(aeIngress, host, 41021, FORWARDER_KEY), true);
    ClientSettings meSettings = coreSettings(meIngress, host, 41022, ME_GATEWAY_KEY);
    meSettings.ingressChannel = "aeron:udp";
    Session engine(aeron, meSettings, false);
    core.client.connect();
    engine.client.connect();
    runUntil({ &core, &engine }, [&] { return core.registrar->isRegistered() && engine.client.isConnected() &&
        engine.meHeartbeats > 0; }, 30'000);
    check(core.registrar->isRegistered(), "core session registered (FORWARDER)");
    check(engine.client.isConnected(), std::string("ME session connected (") + nameOf(engine.client.state()) + " " +
        engine.client.lastDetail() + ")");
    check(engine.meHeartbeats > 0, "ME egress flows on the same aeron client: " + std::to_string(engine.meFrames) +
        " ME frames, " + std::to_string(engine.meHeartbeats) + " ClusterHeartbeat");
    check(engine.heartbeatVersion == 12 && engine.heartbeatBlock == 17 && engine.heartbeatHasEpoch,
        "ClusterHeartbeat arrives at order-schema v12, block 17, and decodes meEpoch (got v" +
        std::to_string(engine.heartbeatVersion) + " block " + std::to_string(engine.heartbeatBlock) + ")");
    // Both alive for 12 s: past the session timeout on both clusters.
    runUntil({ &core, &engine }, [&] { return false; }, 12'000);
    check(core.client.isConnected() && engine.client.isConnected(), "both sessions alive after 12 s");
    check(core.transport.aeron().clientId() == engine.transport.aeron().clientId(), "one Aeron client id for both");
    printCounters("core", core.client);
    printCounters("me", engine.client);
    core.client.close();
    engine.client.close();
    return finish(*monitor);
}

int scenarioLeaderKill(const std::string &dir, const std::string &ingress, const std::string &host)
{
    auto monitor = std::make_shared<AeronFatalMonitor>();
    auto aeron = aeronFor(dir, monitor);
    Session s(aeron, coreSettings(ingress, host, 41031, FORWARDER_KEY), true);
    s.client.connect();
    runUntil({ &s }, [&] { return s.registrar->isRegistered(); }, 30'000);
    check(s.registrar->isRegistered(), "registered before the kill");
    const std::int64_t sessionId = s.client.clusterSessionId();
    const std::int64_t term = s.client.leadershipTermId();
    const std::int32_t leader = s.client.leaderMemberId();
    const std::uint64_t acksBefore = s.registrar->counters().acksAccepted + s.registrar->counters().acksDuplicate;

    // A harmless logged query (QueryFeedPosition, allowed for FORWARDER) every 20 ms so offers
    // are in flight when the leader dies.
    std::uint8_t query[64];
    money::QueryFeedPosition q;
    q.wrapAndApplyHeader(reinterpret_cast<char *>(query), 0, sizeof(query)).correlationId(1);
    const std::size_t queryLength = money::MessageHeader::encodedLength() + q.encodedLength();
    std::int64_t nextQuery = 0;
    auto offerQueries = [&]
    {
        if (nowNs() >= nextQuery)
        {
            timed("tryOffer", [&] { return s.client.tryOffer(query, queryLength); });
            nextQuery = nowNs() + 20'000'000;
        }
    };

    std::printf("READY leader=%d term=%lld session=%lld\n", leader, static_cast<long long>(term),
        static_cast<long long>(sessionId));
    std::fflush(stdout);

    const std::int64_t killWindowStart = nowNs();
    runUntil({ &s }, [&]
        {
            offerQueries();
            return !s.observer.newLeaders.empty() && s.client.isConnected() && s.registrar->isRegistered();
        }, 30'000);
    const double recoveredAfter = static_cast<double>(nowNs() - killWindowStart) / 1e9;
    check(!s.observer.newLeaders.empty(), "sessionSurvivesLeaderKill: NewLeaderEvent received");
    check(s.client.isConnected() && s.client.clusterSessionId() == sessionId,
        "same clusterSessionId " + std::to_string(sessionId) + " continues (" + nameOf(s.client.state()) + ")");
    check(s.client.leadershipTermId() > term && s.client.leaderMemberId() != leader,
        "new term " + std::to_string(s.client.leadershipTermId()) + " and leader " +
        std::to_string(s.client.leaderMemberId()));
    check(s.client.sessionEpoch() == 1, "no reconnect: the session survived the election");
    check(s.registrar->isRegistered() &&
        s.registrar->counters().acksAccepted + s.registrar->counters().acksDuplicate > acksBefore,
        "re-registered in the new term: the new leader answered a RegisterSession stamped with the new term");
    if (!s.observer.newLeaders.empty())
    {
        check(s.observer.newLeaders[0].offersInEndedTerm > 0,
            "staleTermOfferIsDetected: offers stamped with the ended term reported (" +
            std::to_string(s.observer.newLeaders[0].offersInEndedTerm) + ")");
    }
    std::printf("  recovered %.2f s after READY\n", recoveredAfter);
    printCounters("leader-kill", s.client);

    // Phase 2: the run script kills the remaining members. The egress image goes (or ingress
    // reports NOT_CONNECTED), AWAIT_NEW_LEADER, then CLOSED at the 2 s new-leader deadline.
    std::printf("PHASE2\n");
    std::fflush(stdout);
    bool sawAwait = false;
    runUntil({ &s }, [&]
        {
            offerQueries();
            sawAwait = sawAwait || s.client.state() == ClientState::AWAIT_NEW_LEADER;
            return s.client.state() == ClientState::CLOSED;
        }, 40'000);
    check(sawAwait, "all members dead -> AWAIT_NEW_LEADER");
    check(s.client.state() == ClientState::CLOSED && s.client.closeReason() == CloseReason::NEW_LEADER_TIMEOUT,
        std::string("-> CLOSED NEW_LEADER_TIMEOUT (") + nameOf(s.client.closeReason()) + ": " +
        s.client.lastDetail() + ")");
    printCounters("dead-cluster", s.client);
    return finish(*monitor);
}

int scenarioFatal(const std::string &dir)
{
    auto monitor = std::make_shared<AeronFatalMonitor>(); // default action: stderr + _Exit(70)
    AeronClientOptions options;
    options.aeronDir = dir;
    options.clientName = "ct-fatal-probe";
    options.driverTimeoutMs = 1'000;
    auto aeron = connectAeron(options, monitor);
    std::printf("READY\n");
    std::fflush(stdout);
    // A wedged owner: does nothing but sleep. The conductor thread must still end the process.
    std::this_thread::sleep_for(std::chrono::seconds(30));
    std::printf("FAIL still alive 30 s after start\n");
    return 1;
}

}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: see the header of ClientIntegration.cpp\n");
        return 64;
    }
    const std::string scenario = argv[1];
    try
    {
        if (scenario == "auth" && argc == 5) return scenarioAuth(argv[2], argv[3], argv[4]);
        if (scenario == "keepalive" && argc == 5) return scenarioKeepAlive(argv[2], argv[3], argv[4]);
        if (scenario == "two-clusters" && argc == 6) return scenarioTwoClusters(argv[2], argv[3], argv[4], argv[5]);
        if (scenario == "leader-kill" && argc == 5) return scenarioLeaderKill(argv[2], argv[3], argv[4]);
        if (scenario == "fatal" && argc == 3) return scenarioFatal(argv[2]);
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "scenario %s threw: %s\n", scenario.c_str(), e.what());
        return 3;
    }
    std::fprintf(stderr, "unknown scenario or wrong argument count: %s\n", scenario.c_str());
    return 64;
}
