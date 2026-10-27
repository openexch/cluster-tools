// CoreSessionRegistrar over a ClusterClient on the FakeTransport: RegisterSession wire bytes,
// AdminAck handling, the W12 re-send of a lost ack, re-declaration on a new leader and on a
// new session, REJECTED as terminal, back-pressure retried.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <vector>

#include "cluster_tools/client/ClusterClient.h"
#include "cluster_tools/client/CoreSessionRegistrar.h"
#include "FakeTransport.h"

#include "com_openexchange_assets_infrastructure_generated/AdminAck.h"
#include "com_openexchange_assets_infrastructure_generated/MessageHeader.h"
#include "com_openexchange_assets_infrastructure_generated/RegisterSession.h"

using namespace cluster_tools::client;
using cluster_tools::client::test::FakeTransport;
namespace cc = io::aeron::cluster::codecs;
namespace money = com::openexchange::assets::infrastructure::generated;

namespace
{

constexpr std::int64_t MS = 1'000'000LL;
const std::string ENDPOINTS = "0=10.0.0.1:9302,1=10.0.0.2:9302";
using Client = ClusterClient<FakeTransport>;

ClientSettings settings()
{
    ClientSettings s;
    s.ingressEndpoints = ENDPOINTS;
    s.responseChannel = "aeron:udp?endpoint=127.0.0.1:0";
    s.credentials = std::vector<std::uint8_t>(38, 1);
    return s;
}

RegistrarSettings forwarder()
{
    RegistrarSettings r;
    r.sessionClass = SessionClass::FORWARDER;
    r.credentialClass = SessionClass::FORWARDER;
    r.channels = core_channel::DISPATCH | core_channel::CONTROL;
    r.identity = 0xF0F0;
    return r;
}

void drive(Client &client, int rounds = 20)
{
    for (int i = 0; i < rounds; i++)
    {
        client.doWork();
    }
}

void connect(FakeTransport &fake, Client &client, std::int64_t sessionId = 77, std::int64_t term = 5)
{
    REQUIRE(client.connect());
    drive(client);
    fake.pushSessionEvent(fake.lastConnectCorrelation(), sessionId, term, 0, cc::EventCode::OK, "", 900);
    drive(client);
    REQUIRE(client.isConnected());
}

std::vector<std::vector<std::uint8_t>> registerFrames(const FakeTransport &fake)
{
    std::vector<std::vector<std::uint8_t>> out;
    for (const auto &p : fake.publications)
    {
        for (const auto &frame : p.offered)
        {
            if (frame.size() > 40)
            {
                money::MessageHeader h(const_cast<char *>(reinterpret_cast<const char *>(frame.data())), 32, frame.size(), 0);
                if (h.schemaId() == 2 && h.templateId() == 80)
                {
                    out.push_back(frame);
                }
            }
        }
    }
    return out;
}

std::vector<std::uint8_t> adminAck(std::int64_t correlationId, money::AdminResult::Value result, std::uint16_t ref = 80)
{
    std::vector<std::uint8_t> bytes(64);
    money::AdminAck ack;
    ack.wrapAndApplyHeader(reinterpret_cast<char *>(bytes.data()), 0, bytes.size())
        .correlationId(correlationId)
        .refTemplateId(ref)
        .result(result)
        .reason(3)
        .configVersion(0);
    bytes.resize(money::MessageHeader::encodedLength() + ack.encodedLength());
    return bytes;
}

template<typename Registrar>
void pollInto(Client &client, Registrar &registrar)
{
    client.poll([&](const EgressMessage &m) { registrar.onEgress(m); }, 10);
}

}

TEST_CASE("registers once connected; the frame is RegisterSession v-current with the configured fields")
{
    FakeTransport fake;
    Client client(fake, settings());
    CoreSessionRegistrar<Client> registrar(client, forwarder());
    CHECK(registrar.validateSettings().empty());
    registrar.doWork();
    CHECK(registerFrames(fake).empty()); // not connected yet
    connect(fake, client);
    registrar.doWork();
    auto frames = registerFrames(fake);
    REQUIRE(frames.size() == 1);
    char *p = reinterpret_cast<char *>(frames[0].data());
    money::MessageHeader h(p, 32, frames[0].size(), 0);
    CHECK(h.version() == money::MessageHeader::sbeSchemaVersion());
    CHECK(h.version() == 7); // RegisterSession goes out at money-schema v7 (the core refuses a lower header)
    money::RegisterSession r(p, 40, frames[0].size(), h.blockLength(), h.version());
    CHECK(r.correlationId() == registrar.correlationId());
    CHECK(r.sessionClass() == money::SessionClass::FORWARDER);
    CHECK(r.channels().dispatch());
    CHECK(r.channels().control());
    CHECK_FALSE(r.channels().balances());
    CHECK(r.resumable() == money::BoolFlag::TRUE);
    CHECK(r.identity() == 0xF0F0);
    CHECK(registrar.state() == RegistrationState::PENDING);

    fake.pushAppMessage(77, adminAck(registrar.correlationId(), money::AdminResult::ACCEPTED), 900);
    pollInto(client, registrar);
    CHECK(registrar.isRegistered());
    CHECK(registrar.counters().acksAccepted == 1);
}

TEST_CASE("adminAckLostIsResent: no ack within 5 s re-sends the same correlation id; a late DUPLICATE resolves")
{
    FakeTransport fake;
    Client client(fake, settings());
    CoreSessionRegistrar<Client> registrar(client, forwarder());
    connect(fake, client);
    registrar.doWork();
    REQUIRE(registerFrames(fake).size() == 1);
    fake.now += 4999 * MS;
    registrar.doWork();
    CHECK(registerFrames(fake).size() == 1);
    fake.now += 1 * MS;
    registrar.doWork();
    const auto frames = registerFrames(fake);
    REQUIRE(frames.size() == 2);
    CHECK(registrar.counters().resends == 1);
    CHECK(std::equal(frames[0].begin(), frames[0].end(), frames[1].begin())); // identical bytes
    fake.pushAppMessage(77, adminAck(registrar.correlationId(), money::AdminResult::DUPLICATE), 900);
    pollInto(client, registrar);
    CHECK(registrar.isRegistered());
    fake.now += 10'000 * MS;
    registrar.doWork();
    CHECK(registerFrames(fake).size() == 2); // nothing more once registered
}

TEST_CASE("REJECTED is terminal: no re-send, the owner must exit")
{
    FakeTransport fake;
    Client client(fake, settings());
    CoreSessionRegistrar<Client> registrar(client, forwarder());
    connect(fake, client);
    registrar.doWork();
    fake.pushAppMessage(77, adminAck(registrar.correlationId(), money::AdminResult::REJECTED), 900);
    pollInto(client, registrar);
    CHECK(registrar.isRejected());
    CHECK(registrar.detail().find("REJECTED") != std::string::npos);
    fake.now += 60'000 * MS;
    registrar.doWork();
    CHECK(registerFrames(fake).size() == 1);
}

TEST_CASE("acks for another correlation id or another template are not ours")
{
    FakeTransport fake;
    Client client(fake, settings());
    CoreSessionRegistrar<Client> registrar(client, forwarder());
    connect(fake, client);
    registrar.doWork();
    fake.pushAppMessage(77, adminAck(registrar.correlationId() + 1, money::AdminResult::ACCEPTED), 900);
    fake.pushAppMessage(77, adminAck(registrar.correlationId(), money::AdminResult::ACCEPTED, 73), 900);
    pollInto(client, registrar);
    CHECK(registrar.state() == RegistrationState::PENDING);
    CHECK(registrar.counters().acksIgnored == 2);
}

TEST_CASE("a new leader voids the registration: re-declared in the new term with a new correlation id")
{
    FakeTransport fake;
    Client client(fake, settings());
    CoreSessionRegistrar<Client> registrar(client, forwarder());
    connect(fake, client);
    registrar.doWork();
    const std::int64_t first = registrar.correlationId();
    fake.pushAppMessage(77, adminAck(first, money::AdminResult::ACCEPTED), 900);
    pollInto(client, registrar);
    REQUIRE(registrar.isRegistered());

    fake.pushNewLeader(77, 6, 1, ENDPOINTS, 950);
    pollInto(client, registrar);
    CHECK_FALSE(registrar.isRegistered());
    client.doWork(); // keepalive -> CONNECTED in term 6
    REQUIRE(client.isConnected());
    registrar.doWork();
    const auto frames = registerFrames(fake);
    REQUIRE(frames.size() == 2);
    std::int64_t term;
    std::memcpy(&term, frames[1].data() + 8, sizeof(term));
    CHECK(term == 6);
    CHECK(registrar.correlationId() != first);
    fake.pushAppMessage(77, adminAck(registrar.correlationId(), money::AdminResult::DUPLICATE), 951);
    pollInto(client, registrar);
    CHECK(registrar.isRegistered());
}

TEST_CASE("a new session (reconnect) re-registers")
{
    FakeTransport fake;
    Client client(fake, settings());
    CoreSessionRegistrar<Client> registrar(client, forwarder());
    connect(fake, client);
    registrar.doWork();
    fake.pushAppMessage(77, adminAck(registrar.correlationId(), money::AdminResult::ACCEPTED), 900);
    pollInto(client, registrar);
    REQUIRE(registrar.isRegistered());
    client.close();
    CHECK_FALSE(registrar.isRegistered());
    connect(fake, client, 88, 5);
    registrar.doWork();
    CHECK(registerFrames(fake).size() == 2);
    CHECK(registrar.state() == RegistrationState::PENDING);
}

TEST_CASE("a back-pressured RegisterSession is retried on the next duty cycle")
{
    FakeTransport fake;
    Client client(fake, settings());
    CoreSessionRegistrar<Client> registrar(client, forwarder());
    connect(fake, client);
    fake.publications[static_cast<std::size_t>(fake.publicationTo("10.0.0.1:9302"))].scriptedResults =
        { OFFER_BACK_PRESSURED };
    registrar.doWork();
    CHECK(registerFrames(fake).empty());
    CHECK(registrar.state() == RegistrationState::UNREGISTERED);
    registrar.doWork();
    CHECK(registerFrames(fake).size() == 1);
    CHECK(registrar.counters().sendBackPressured == 1);
}

TEST_CASE("settings: class required, resumable=false refused")
{
    FakeTransport fake;
    Client client(fake, settings());
    RegistrarSettings bad;
    CHECK_FALSE(CoreSessionRegistrar<Client>(client, bad).validateSettings().empty());
    RegistrarSettings notResumable = forwarder();
    notResumable.resumable = false;
    CHECK_FALSE(CoreSessionRegistrar<Client>(client, notResumable).validateSettings().empty());
}

// ---- added by the S11 independent review (notebook .../RC2-me-link/S11/review.md) ----

namespace
{
std::int64_t correlationOf(const std::vector<std::uint8_t> &frame)
{
    char *p = const_cast<char *>(reinterpret_cast<const char *>(frame.data()));
    money::MessageHeader h(p, 32, frame.size(), 0);
    money::RegisterSession r(p, 40, frame.size(), h.blockLength(), h.version());
    return r.correlationId();
}
}

TEST_CASE("review: every re-send of one round, including one delayed by back-pressure, keeps the correlation id; the first send's ack resolves it")
{
    FakeTransport fake;
    Client client(fake, settings());
    CoreSessionRegistrar<Client> registrar(client, forwarder());
    connect(fake, client);
    registrar.doWork();
    const std::int64_t id = registrar.correlationId();
    auto &pub = fake.publications[static_cast<std::size_t>(fake.publicationTo("10.0.0.1:9302"))];
    for (int round = 0; round < 3; round++)
    {
        fake.now += 5000 * MS;
        pub.scriptedResults = { OFFER_BACK_PRESSURED };
        registrar.doWork(); // due, back-pressured
        registrar.doWork(); // retried at once
    }
    const auto frames = registerFrames(fake);
    REQUIRE(frames.size() == 4);
    for (const auto &frame : frames)
    {
        CHECK(correlationOf(frame) == id);
    }
    CHECK(registrar.counters().resends == 3);
    CHECK(registrar.counters().rounds == 1);
    fake.pushAppMessage(77, adminAck(id, money::AdminResult::ACCEPTED), 900);
    pollInto(client, registrar);
    CHECK(registrar.isRegistered());
}

TEST_CASE("review: REJECTED stays terminal across a new leader and a new session; a later ACCEPTED does not revive it")
{
    FakeTransport fake;
    Client client(fake, settings());
    CoreSessionRegistrar<Client> registrar(client, forwarder());
    connect(fake, client);
    registrar.doWork();
    const std::int64_t id = registrar.correlationId();
    fake.pushAppMessage(77, adminAck(id, money::AdminResult::REJECTED), 900);
    fake.pushAppMessage(77, adminAck(id, money::AdminResult::ACCEPTED), 900);
    pollInto(client, registrar);
    CHECK(registrar.isRejected());
    CHECK_FALSE(registrar.isRegistered());

    fake.pushNewLeader(77, 6, 1, ENDPOINTS, 950);
    pollInto(client, registrar);
    client.doWork();
    REQUIRE(client.isConnected());
    registrar.doWork();
    CHECK(registerFrames(fake).size() == 1);

    client.close();
    connect(fake, client, 88, 6);
    fake.now += 60'000 * MS;
    registrar.doWork();
    CHECK(registerFrames(fake).size() == 1);
    CHECK(registrar.isRejected());
    CHECK(registrar.state() == RegistrationState::REJECTED);
}

TEST_CASE("review: an ack of the previous term's round does not register the new term")
{
    FakeTransport fake;
    Client client(fake, settings());
    CoreSessionRegistrar<Client> registrar(client, forwarder());
    connect(fake, client);
    registrar.doWork();
    const std::int64_t first = registrar.correlationId();
    fake.pushNewLeader(77, 6, 1, ENDPOINTS, 950);
    pollInto(client, registrar);
    client.doWork();
    registrar.doWork();
    REQUIRE(registrar.correlationId() != first);
    fake.pushAppMessage(77, adminAck(first, money::AdminResult::ACCEPTED), 951); // late, old round
    pollInto(client, registrar);
    CHECK_FALSE(registrar.isRegistered());
    CHECK(registrar.state() == RegistrationState::PENDING);
}

// ---- review fixes F3 (lost REJECTED, class mismatch) ----

namespace
{

// The core closes a refused session through consensus; the REJECTED ack is lost with its queue.
void closeByCluster(FakeTransport &fake, Client &client, std::int64_t sessionId)
{
    fake.pushSessionEvent(0, sessionId, 5, 0, cc::EventCode::CLOSED, "service action", 900);
    client.poll([](const EgressMessage &) {}, 10);
    REQUIRE(client.state() == ClientState::CLOSED);
}

}

TEST_CASE("F3: a class that differs from the credential's class is never sent")
{
    FakeTransport fake;
    Client client(fake, settings());
    RegistrarSettings mismatch = forwarder();
    mismatch.credentialClass = SessionClass::BRIDGE;
    CoreSessionRegistrar<Client> registrar(client, mismatch);
    connect(fake, client);
    registrar.doWork();
    CHECK(registerFrames(fake).empty());
    CHECK(registrar.detail().find("BRIDGE") != std::string::npos);
    CHECK(registrar.state() == RegistrationState::UNREGISTERED);
}

TEST_CASE("F3: a lost REJECTED (session closed while pending) three times in a row is terminal")
{
    FakeTransport fake;
    Client client(fake, settings());
    CoreSessionRegistrar<Client> registrar(client, forwarder());
    for (int round = 0; round < 3; round++)
    {
        connect(fake, client, 100 + round, 5);
        registrar.doWork();
        REQUIRE(registrar.state() == RegistrationState::PENDING);
        closeByCluster(fake, client, 100 + round);
        registrar.doWork();
        CHECK(registrar.consecutiveUnansweredSessions() == round + 1);
    }
    CHECK(registrar.isRejected());
    CHECK(registrar.detail().find("unanswered") != std::string::npos);
    CHECK(registrar.counters().closesWhilePending == 3);
    const std::size_t sent = registerFrames(fake).size();
    connect(fake, client, 200, 5);
    registrar.doWork();
    CHECK(registerFrames(fake).size() == sent); // terminal: no further registration
}

TEST_CASE("F3: an accepted registration resets the unanswered count; a client close does not count")
{
    FakeTransport fake;
    Client client(fake, settings());
    CoreSessionRegistrar<Client> registrar(client, forwarder());
    connect(fake, client, 100, 5);
    registrar.doWork();
    closeByCluster(fake, client, 100);
    registrar.doWork();
    registrar.doWork(); // counted once per session
    CHECK(registrar.consecutiveUnansweredSessions() == 1);

    connect(fake, client, 101, 5);
    registrar.doWork();
    fake.pushAppMessage(101, adminAck(registrar.correlationId(), money::AdminResult::ACCEPTED), 900);
    pollInto(client, registrar);
    REQUIRE(registrar.isRegistered());
    CHECK(registrar.consecutiveUnansweredSessions() == 0);

    client.close(); // the owner's own close is not a refusal
    connect(fake, client, 102, 5);
    registrar.doWork();
    client.close();
    registrar.doWork();
    CHECK(registrar.consecutiveUnansweredSessions() == 0);
    CHECK_FALSE(registrar.isRejected());
}

TEST_CASE("RC2 S12: requestReRegistration starts a new round in the same session and term; REJECTED stays terminal")
{
    FakeTransport fake;
    Client client(fake, settings());
    CoreSessionRegistrar<Client> registrar(client, forwarder());
    connect(fake, client);
    registrar.doWork();
    const std::int64_t first = registrar.correlationId();
    fake.pushAppMessage(77, adminAck(first, money::AdminResult::ACCEPTED), 900);
    pollInto(client, registrar);
    REQUIRE(registrar.isRegistered());
    // the core said UNREGISTERED_SESSION naming one of our commands (S12 4.1 C3 -> C2)
    registrar.requestReRegistration();
    CHECK_FALSE(registrar.isRegistered());
    CHECK(registrar.correlationId() != first);
    registrar.doWork();
    const auto frames = registerFrames(fake);
    REQUIRE(frames.size() == 2);
    CHECK(correlationOf(frames[1]) == registrar.correlationId());
    fake.pushAppMessage(77, adminAck(first, money::AdminResult::ACCEPTED), 900); // the old round's ack: not ours
    pollInto(client, registrar);
    CHECK_FALSE(registrar.isRegistered());
    fake.pushAppMessage(77, adminAck(registrar.correlationId(), money::AdminResult::DUPLICATE), 900);
    pollInto(client, registrar);
    CHECK(registrar.isRegistered());
    fake.pushAppMessage(77, adminAck(registrar.correlationId(), money::AdminResult::REJECTED), 900);
    pollInto(client, registrar);
    REQUIRE(registrar.isRejected());
    registrar.requestReRegistration();
    CHECK(registrar.isRejected());
}
