// ClusterClient state machine against the scripted FakeTransport: connect, redirect,
// typed connect failures, bounded retries with backoff, keepalive cadence, leader change,
// egress image loss, back-pressure, close and reconnect. No Aeron, no clock but the fake's.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <string>
#include <vector>

#include "cluster_tools/client/ClusterClient.h"
#include "FakeTransport.h"

using namespace cluster_tools::client;
using cluster_tools::client::test::FakeTransport;
namespace cc = io::aeron::cluster::codecs;

namespace
{

constexpr std::int64_t MS = 1'000'000LL;
const std::string ENDPOINTS = "0=10.0.0.1:9302,1=10.0.0.2:9302,2=10.0.0.3:9302";
const std::string DETAIL_ENDPOINTS = ENDPOINTS;

struct Recorder
{
    std::vector<std::pair<ClientState, ClientState>> transitions;
    std::vector<NewLeaderInfo> newLeaders;
    std::vector<SessionEventInfo> events;
    void onStateChange(ClientState from, ClientState to) { transitions.emplace_back(from, to); }
    void onNewLeader(const NewLeaderInfo &info) { newLeaders.push_back(info); }
    void onSessionEvent(const SessionEventInfo &info) { events.push_back(info); }
};

using Client = ClusterClient<FakeTransport, Recorder>;

ClientSettings settings()
{
    ClientSettings s;
    s.ingressEndpoints = ENDPOINTS;
    s.responseChannel = "aeron:udp?endpoint=127.0.0.1:0";
    s.credentials = { 1, 2, 6, 0, 0, 0, 0x66 };
    s.maxConnectAttempts = 3;
    return s;
}

int drive(Client &client, int rounds = 20)
{
    int work = 0;
    for (int i = 0; i < rounds; i++)
    {
        work += client.doWork();
    }
    return work;
}

std::uint16_t templateOf(const std::vector<std::uint8_t> &frame)
{
    cc::MessageHeader header(const_cast<char *>(reinterpret_cast<const char *>(frame.data())), 0, frame.size(), 0);
    return header.templateId();
}

std::int64_t termOf(const std::vector<std::uint8_t> &frame)
{
    // SessionMessageHeader / SessionKeepAlive / SessionCloseRequest all start with leadershipTermId.
    std::int64_t term;
    std::memcpy(&term, frame.data() + 8, sizeof(term));
    return term;
}

// Drives a fresh client to CONNECTED on member 0 with session 77, term 5.
void connectOk(FakeTransport &fake, Client &client)
{
    REQUIRE(client.connect());
    drive(client);
    PublicationId on = NO_PUBLICATION;
    const std::int64_t correlation = fake.lastConnectCorrelation(&on);
    REQUIRE(correlation > 0);
    fake.pushSessionEvent(correlation, 77, 5, 0, cc::EventCode::OK, "", 900);
    drive(client);
    REQUIRE(client.state() == ClientState::CONNECTED);
}

}

TEST_CASE("connect: request on the first connected member carries credentials and the resolved channel")
{
    FakeTransport fake;
    Recorder recorder;
    Client client(fake, settings(), &recorder);
    fake.deadEndpoints = { "10.0.0.1" }; // member 0 never connects
    REQUIRE(client.connect());
    CHECK(client.state() == ClientState::CONNECTING);
    drive(client);

    PublicationId on = NO_PUBLICATION;
    const std::int64_t correlation = fake.lastConnectCorrelation(&on);
    REQUIRE(correlation > 0);
    CHECK(fake.publications[static_cast<std::size_t>(on)].channel == "aeron:udp?endpoint=10.0.0.2:9302");

    const auto &frame = fake.publications[static_cast<std::size_t>(on)].offered.back();
    cc::SessionConnectRequest request(const_cast<char *>(reinterpret_cast<const char *>(frame.data())), 8,
        frame.size(), cc::SessionConnectRequest::sbeBlockLength(), cc::SessionConnectRequest::sbeSchemaVersion());
    CHECK(request.responseStreamId() == 102);
    CHECK(request.version() == PROTOCOL_SEMANTIC_VERSION);
    CHECK(request.getResponseChannelAsString() == "aeron:udp?endpoint=127.0.0.1:40123");
    const std::string credentials = request.getEncodedCredentialsAsString();
    CHECK(std::vector<std::uint8_t>(credentials.begin(), credentials.end()) == settings().credentials);
    CHECK(fake.egressChannel == "aeron:udp?endpoint=127.0.0.1:0");

    fake.pushSessionEvent(correlation, 77, 5, 1, cc::EventCode::OK, "", 900, 1'000'000'000LL);
    drive(client);
    CHECK(client.state() == ClientState::CONNECTED);
    CHECK(client.clusterSessionId() == 77);
    CHECK(client.leadershipTermId() == 5);
    CHECK(client.leaderMemberId() == 1);
    CHECK(client.sessionEpoch() == 1);
    CHECK(client.newLeaderTimeoutNs() == 2'000'000'000LL); // 2 x the leader's heartbeat timeout
    // Only the winning publication survives the connect.
    int open = 0;
    for (const auto &p : fake.publications)
    {
        open += p.closed ? 0 : 1;
    }
    CHECK(open == 1);
    CHECK_FALSE(fake.publications[static_cast<std::size_t>(on)].closed);
}

TEST_CASE("connect: a follower REDIRECT moves the request to the leader's endpoint")
{
    FakeTransport fake;
    Client client(fake, settings());
    REQUIRE(client.connect());
    drive(client);
    PublicationId first = NO_PUBLICATION;
    const std::int64_t correlation = fake.lastConnectCorrelation(&first);
    fake.pushSessionEvent(correlation, -1, 5, 2, cc::EventCode::REDIRECT, DETAIL_ENDPOINTS, 901);
    drive(client);
    CHECK(client.counters().redirects == 1);

    PublicationId second = NO_PUBLICATION;
    const std::int64_t correlation2 = fake.lastConnectCorrelation(&second);
    REQUIRE(correlation2 != correlation);
    CHECK(fake.publications[static_cast<std::size_t>(second)].channel == "aeron:udp?endpoint=10.0.0.3:9302");
    CHECK(fake.publications[static_cast<std::size_t>(first)].closed);

    fake.pushSessionEvent(correlation2, 78, 5, 2, cc::EventCode::OK, "", 902);
    drive(client);
    CHECK(client.state() == ClientState::CONNECTED);
    CHECK(client.leaderMemberId() == 2);
}

TEST_CASE("connect: AUTHENTICATION_REJECTED is a typed, terminal failure (no retry)")
{
    FakeTransport fake;
    Client client(fake, settings());
    REQUIRE(client.connect());
    drive(client);
    fake.pushSessionEvent(fake.lastConnectCorrelation(), -1, 5, 0, cc::EventCode::AUTHENTICATION_REJECTED,
        "unknown key", 900);
    drive(client);
    CHECK(client.state() == ClientState::FAILED);
    CHECK(client.connectError() == ConnectError::AUTHENTICATION_REJECTED);
    CHECK(client.lastDetail() == "unknown key");
    CHECK(client.counters().connectAttempts == 1);
    CHECK_FALSE(fake.subscriptionOpen);
    CHECK_FALSE(fake.closedSubscriptionInsidePoll);
}

TEST_CASE("connect: cluster ERROR and a challenge are typed terminal failures")
{
    {
        FakeTransport fake;
        Client client(fake, settings());
        REQUIRE(client.connect());
        drive(client);
        fake.pushSessionEvent(fake.lastConnectCorrelation(), -1, 5, 0, cc::EventCode::ERROR, "invalid client version", 900);
        drive(client);
        CHECK(client.connectError() == ConnectError::CLUSTER_ERROR);
        CHECK(client.lastDetail() == "invalid client version");
    }
    {
        FakeTransport fake;
        Client client(fake, settings());
        REQUIRE(client.connect());
        drive(client);
        fake.pushChallenge(fake.lastConnectCorrelation(), 5, 900);
        drive(client);
        CHECK(client.connectError() == ConnectError::CHALLENGE_UNSUPPORTED);
    }
}

TEST_CASE("connect: responses for another correlation id are ignored")
{
    FakeTransport fake;
    Client client(fake, settings());
    REQUIRE(client.connect());
    drive(client);
    fake.pushSessionEvent(fake.lastConnectCorrelation() + 1000, 1, 5, 0, cc::EventCode::OK, "", 900);
    drive(client);
    CHECK(client.state() == ClientState::CONNECTING);
}

TEST_CASE("connect: attempts time out, back off exponentially, and are bounded")
{
    FakeTransport fake;
    ClientSettings s = settings();
    s.maxConnectAttempts = 3;
    s.messageTimeoutNs = 50 * MS;
    s.connectBackoffInitialNs = 100 * MS;
    s.connectBackoffMaxNs = 150 * MS;
    Client client(fake, s);
    REQUIRE(client.connect());
    drive(client);
    CHECK(client.counters().connectAttempts == 1);

    fake.now += 50 * MS; // attempt 1 deadline
    drive(client, 1);
    CHECK(client.counters().connectAttemptsFailed == 1);
    fake.now += 99 * MS;
    drive(client);
    CHECK(client.counters().connectAttempts == 1); // still backing off
    fake.now += 1 * MS;
    drive(client);
    CHECK(client.counters().connectAttempts == 2);

    fake.now += 50 * MS;
    drive(client, 1);
    fake.now += 149 * MS; // capped at 150 ms, not 200
    drive(client);
    CHECK(client.counters().connectAttempts == 2);
    fake.now += 1 * MS;
    drive(client);
    CHECK(client.counters().connectAttempts == 3);

    fake.now += 50 * MS;
    drive(client);
    CHECK(client.state() == ClientState::FAILED);
    CHECK(client.connectError() == ConnectError::ATTEMPTS_EXHAUSTED);
    CHECK(client.counters().connectAttempts == 3);
    CHECK(client.lastDetail().find("attempt 3/3") != std::string::npos);
}

TEST_CASE("connect: never waits inside a call; a pending driver registration is just another step")
{
    FakeTransport fake;
    fake.subscriptionStatus = Registration::PENDING;
    Client client(fake, settings());
    REQUIRE(client.connect());
    for (int i = 0; i < 100; i++)
    {
        client.doWork();
    }
    CHECK(client.state() == ClientState::CONNECTING);
    CHECK(fake.offerCalls == 0);
    fake.subscriptionStatus = Registration::READY;
    drive(client);
    CHECK(fake.lastConnectCorrelation() > 0);
}

TEST_CASE("settings: missing credentials or a bad response channel refuse to connect")
{
    FakeTransport fake;
    ClientSettings s = settings();
    s.credentials.clear();
    Client client(fake, s);
    CHECK_FALSE(client.connect());
    CHECK(client.state() == ClientState::FAILED);
    CHECK(client.connectError() == ConnectError::INVALID_SETTINGS);

    ClientSettings anonymous = settings();
    anonymous.credentials.clear();
    anonymous.allowEmptyCredentials = true;
    CHECK(validate(anonymous).empty());

    ClientSettings wildcard = settings();
    wildcard.responseChannel = "aeron:udp?endpoint=0.0.0.0:0";
    CHECK_FALSE(validate(wildcard).empty());
    ClientSettings slowKeepAlive = settings();
    slowKeepAlive.keepAliveIntervalNs = 2'000'000'000LL;
    CHECK_FALSE(validate(slowKeepAlive).empty());
}

TEST_CASE("offer: back-pressure returns at once, one offer, no retry, state unchanged")
{
    FakeTransport fake;
    Client client(fake, settings());
    connectOk(fake, client);
    const PublicationId pub = fake.publicationTo("10.0.0.1:9302");
    fake.publications[static_cast<std::size_t>(pub)].scriptedResults = { OFFER_BACK_PRESSURED, OFFER_ADMIN_ACTION };
    const int before = fake.offerCalls;
    const std::uint8_t payload[4] = { 1, 2, 3, 4 };
    OfferResult r = client.tryOffer(payload, sizeof(payload));
    CHECK(r.status == OfferStatus::BACK_PRESSURED);
    CHECK(fake.offerCalls == before + 1);
    r = client.tryOffer(payload, sizeof(payload));
    CHECK(r.status == OfferStatus::BACK_PRESSURED);
    CHECK(client.state() == ClientState::CONNECTED);
    r = client.tryOffer(payload, sizeof(payload));
    CHECK(r.ok());
    CHECK(client.counters().offersBackPressured == 2);
    const auto &frame = fake.publications[static_cast<std::size_t>(pub)].offered.back();
    CHECK(frame.size() == 32 + 4);
    CHECK(templateOf(frame) == cc::SessionMessageHeader::sbeTemplateId());
    CHECK(termOf(frame) == 5);
    CHECK(frame[32] == 1);
}

TEST_CASE("offer: NOT_READY outside a session")
{
    FakeTransport fake;
    Client client(fake, settings());
    const std::uint8_t payload[1] = { 1 };
    CHECK(client.tryOffer(payload, 1).status == OfferStatus::NOT_READY);
    REQUIRE(client.connect());
    CHECK(client.tryOffer(payload, 1).status == OfferStatus::NOT_READY);
}

TEST_CASE("keepalive: every interval from doWork alone; a failed one is retried soon, not a second later")
{
    FakeTransport fake;
    Client client(fake, settings());
    connectOk(fake, client);
    auto &offered = fake.publications[static_cast<std::size_t>(fake.publicationTo("10.0.0.1:9302"))].offered;
    const std::size_t base = offered.size();
    const int polls = fake.pollCalls;

    fake.now += 999 * MS;
    client.doWork();
    CHECK(offered.size() == base);
    fake.now += 1 * MS;
    client.doWork();
    REQUIRE(offered.size() == base + 1);
    CHECK(templateOf(offered.back()) == cc::SessionKeepAlive::sbeTemplateId());
    CHECK(termOf(offered.back()) == 5);
    CHECK(fake.pollCalls == polls); // independent of poll

    // A back-pressured keepalive does not advance the schedule.
    auto &pub = fake.publications[static_cast<std::size_t>(fake.publicationTo("10.0.0.1:9302"))];
    pub.scriptedResults = { OFFER_BACK_PRESSURED };
    fake.now += 1000 * MS;
    client.doWork();
    CHECK(offered.size() == base + 1);
    CHECK(client.counters().keepAliveFailures == 1);
    fake.now += 10 * MS;
    client.doWork();
    CHECK(offered.size() == base + 2);
    CHECK(client.counters().keepAlivesSent == 2);

    // Over 10 simulated seconds of doWork-only operation: never a gap above 1 s.
    std::int64_t last = fake.now;
    std::size_t count = offered.size();
    std::int64_t maxGap = 0;
    for (int i = 0; i < 10'000; i++)
    {
        fake.now += 1 * MS;
        client.doWork();
        if (offered.size() != count)
        {
            maxGap = std::max(maxGap, fake.now - last);
            last = fake.now;
            count = offered.size();
        }
    }
    CHECK(maxGap <= 1000 * MS);
    CHECK(client.counters().keepAlivesSent >= 11);
}

TEST_CASE("keepalive: the test-only switch silences it (control for the 10 s session timeout)")
{
    FakeTransport fake;
    ClientSettings s = settings();
    s.disableKeepAliveForTest = true;
    Client client(fake, s);
    connectOk(fake, client);
    fake.now += 5000 * MS;
    drive(client);
    CHECK(client.counters().keepAlivesSent == 0);
}

TEST_CASE("leader change: new term stamped, ingress re-pointed, keepalive at once, CONNECTED on first positive offer")
{
    FakeTransport fake;
    Recorder recorder;
    Client client(fake, settings(), &recorder);
    connectOk(fake, client);
    const PublicationId oldPub = fake.publicationTo("10.0.0.1:9302");
    const std::uint8_t payload[2] = { 9, 9 };
    REQUIRE(client.tryOffer(payload, 2).ok());
    REQUIRE(client.tryOffer(payload, 2).ok());
    fake.pushAppMessage(77, { 1 }, 900); // an egress frame after the first two offers...
    client.poll([](const EgressMessage &) {}, 10);
    REQUIRE(client.tryOffer(payload, 2).ok()); // ...and one more after it

    fake.autoConnect = false; // the new leader's publication is not connected yet
    fake.pushNewLeader(77, 6, 2, DETAIL_ENDPOINTS, 950);
    client.poll([](const EgressMessage &) {}, 10);
    CHECK(client.state() == ClientState::AWAIT_NEW_LEADER_CONNECTION);
    CHECK(client.leadershipTermId() == 6);
    CHECK(client.leaderMemberId() == 2);
    CHECK(fake.publications[static_cast<std::size_t>(oldPub)].closed);
    CHECK(fake.assemblyResets == 1);
    CHECK(fake.resetsInsidePoll == 1); // at the event, like AeronCluster.onNewLeader, not after the poll
    REQUIRE(recorder.newLeaders.size() == 1);
    CHECK(recorder.newLeaders[0].previousLeadershipTermId == 5);
    CHECK(recorder.newLeaders[0].offersInEndedTerm == 3);
    CHECK(recorder.newLeaders[0].offersAfterLastEgress == 1);
    CHECK(client.counters().possiblyLostOffers == 1);

    const PublicationId newPub = fake.publicationTo("10.0.0.3:9302");
    REQUIRE(newPub != NO_PUBLICATION);
    CHECK(client.tryOffer(payload, 2).status == OfferStatus::NOT_CONNECTED);
    CHECK(client.state() == ClientState::AWAIT_NEW_LEADER_CONNECTION); // NOT_CONNECTED is not terminal here

    fake.publications[static_cast<std::size_t>(newPub)].connected = true;
    client.doWork(); // keepalive is due immediately on a new leader
    CHECK(client.state() == ClientState::CONNECTED);
    const auto &frames = fake.publications[static_cast<std::size_t>(newPub)].offered;
    REQUIRE(frames.size() == 1);
    CHECK(templateOf(frames[0]) == cc::SessionKeepAlive::sbeTemplateId());
    CHECK(termOf(frames[0]) == 6);
    REQUIRE(client.tryOffer(payload, 2).ok());
    CHECK(termOf(frames.back()) == 6);
    CHECK(client.clusterSessionId() == 77);
    CHECK(client.sessionEpoch() == 1); // same session survived
}

TEST_CASE("leader change: no positive offer to the new leader within messageTimeout closes the session")
{
    FakeTransport fake;
    Client client(fake, settings());
    connectOk(fake, client);
    fake.autoConnect = false;
    fake.pushNewLeader(77, 6, 2, DETAIL_ENDPOINTS, 950);
    client.poll([](const EgressMessage &) {}, 10);
    fake.now += 4999 * MS;
    drive(client);
    CHECK(client.state() == ClientState::AWAIT_NEW_LEADER_CONNECTION);
    fake.now += 1 * MS;
    drive(client);
    CHECK(client.state() == ClientState::CLOSED);
    CHECK(client.closeReason() == CloseReason::NEW_LEADER_CONNECTION_TIMEOUT);
}

TEST_CASE("egress image loss: AWAIT_NEW_LEADER, then CLOSED at 2 x leader heartbeat timeout")
{
    FakeTransport fake;
    Client client(fake, settings());
    connectOk(fake, client);
    fake.closedImages[900] = true;
    client.poll([](const EgressMessage &) {}, 10);
    CHECK(client.state() == ClientState::AWAIT_NEW_LEADER);
    CHECK(client.counters().egressImageLosses == 1);
    const std::uint8_t payload[1] = { 1 };
    CHECK(client.tryOffer(payload, 1).status == OfferStatus::NOT_READY);
    fake.now += 1999 * MS;
    drive(client);
    CHECK(client.state() == ClientState::AWAIT_NEW_LEADER);
    fake.now += 1 * MS;
    drive(client);
    CHECK(client.state() == ClientState::CLOSED);
    CHECK(client.closeReason() == CloseReason::NEW_LEADER_TIMEOUT);
}

TEST_CASE("egress image loss followed by a NewLeaderEvent on the new image: the session survives")
{
    FakeTransport fake;
    Client client(fake, settings());
    connectOk(fake, client);
    fake.closedImages[900] = true;
    client.poll([](const EgressMessage &) {}, 10);
    REQUIRE(client.state() == ClientState::AWAIT_NEW_LEADER);
    fake.now += 1500 * MS;
    fake.pushNewLeader(77, 6, 1, DETAIL_ENDPOINTS, 951);
    client.poll([](const EgressMessage &) {}, 10);
    CHECK(client.state() == ClientState::AWAIT_NEW_LEADER_CONNECTION);
    client.doWork();
    CHECK(client.state() == ClientState::CONNECTED);
    fake.now += 5000 * MS; // the old deadline does not apply any more
    drive(client);
    client.poll([](const EgressMessage &) {}, 10);
    CHECK(client.state() == ClientState::CONNECTED); // image 951 is alive
}

TEST_CASE("ingress NOT_CONNECTED while CONNECTED moves to AWAIT_NEW_LEADER; MAX_POSITION closes")
{
    {
        FakeTransport fake;
        Client client(fake, settings());
        connectOk(fake, client);
        fake.publications[static_cast<std::size_t>(fake.publicationTo("10.0.0.1:9302"))].connected = false;
        const std::uint8_t payload[1] = { 1 };
        CHECK(client.tryOffer(payload, 1).status == OfferStatus::NOT_CONNECTED);
        CHECK(client.state() == ClientState::AWAIT_NEW_LEADER);
    }
    {
        FakeTransport fake;
        Client client(fake, settings());
        connectOk(fake, client);
        fake.publications[static_cast<std::size_t>(fake.publicationTo("10.0.0.1:9302"))].scriptedResults =
            { OFFER_MAX_POSITION_EXCEEDED };
        const std::uint8_t payload[1] = { 1 };
        CHECK(client.tryOffer(payload, 1).status == OfferStatus::MAX_POSITION_EXCEEDED);
        CHECK(client.state() == ClientState::CLOSED);
        CHECK(client.closeReason() == CloseReason::MAX_POSITION_EXCEEDED);
    }
}

TEST_CASE("SessionEvent CLOSED from the cluster closes the session after the poll, never inside it")
{
    FakeTransport fake;
    Recorder recorder;
    Client client(fake, settings(), &recorder);
    connectOk(fake, client);
    fake.pushSessionEvent(0, 77, 5, 0, cc::EventCode::CLOSED, "session timed out", 900);
    fake.pushAppMessage(77, { 5 }, 900);
    int delivered = 0;
    client.poll([&](const EgressMessage &) { delivered++; }, 10);
    CHECK(client.state() == ClientState::CLOSED);
    CHECK(client.closeReason() == CloseReason::SESSION_CLOSED_BY_CLUSTER);
    CHECK(client.lastDetail().find("session timed out") != std::string::npos);
    CHECK_FALSE(fake.closedSubscriptionInsidePoll);
    CHECK(recorder.events.size() == 1);
    CHECK(delivered == 1); // the fragment polled in the same batch is still this session's
}

TEST_CASE("egress: only this session's app messages reach the handler, payload after the session header")
{
    FakeTransport fake;
    Client client(fake, settings());
    connectOk(fake, client);
    fake.pushAppMessage(77, { 0xAB, 0xCD }, 900);
    fake.pushAppMessage(12, { 0xEE }, 900);
    std::vector<std::vector<std::uint8_t>> got;
    client.poll([&](const EgressMessage &m)
        {
            got.emplace_back(m.payload, m.payload + m.length);
            CHECK(m.timestamp == 42);
        }, 10);
    REQUIRE(got.size() == 1);
    CHECK(got[0] == std::vector<std::uint8_t>{ 0xAB, 0xCD });
    CHECK(client.counters().foreignSessionFragments == 1);
}

TEST_CASE("egress: every app message carries the leadership term its SessionMessageHeader was stamped with (RC2 S12 W6)")
{
    FakeTransport fake;
    Recorder recorder;
    Client client(fake, settings(), &recorder);
    connectOk(fake, client);
    fake.pushAppMessage(77, { 1 }, 900, 5);
    fake.pushNewLeader(77, 6, 1, DETAIL_ENDPOINTS, 901);
    fake.pushAppMessage(77, { 2 }, 900, 5); // a deposed leader's frame after the event (FIX-1)
    fake.pushAppMessage(77, { 3 }, 901, 6);
    std::vector<std::pair<std::uint8_t, std::int64_t>> seen;
    std::vector<std::int64_t> clientTermAtDelivery;
    client.poll([&](const EgressMessage &m) {
        seen.emplace_back(m.payload[0], m.leadershipTermId);
        clientTermAtDelivery.push_back(client.leadershipTermId());
    }, 10);
    REQUIRE(seen.size() == 3);
    CHECK(seen[0] == std::make_pair<std::uint8_t, std::int64_t>(1, 5));
    CHECK(seen[1] == std::make_pair<std::uint8_t, std::int64_t>(2, 5));
    CHECK(seen[2] == std::make_pair<std::uint8_t, std::int64_t>(3, 6));
    // what the owner compares against: the client's term moved to 6 at the event
    CHECK(clientTermAtDelivery == std::vector<std::int64_t>{ 5, 6, 6 });
}

TEST_CASE("close: polite SessionCloseRequest, CLOSED/CLIENT_CLOSE, and connect() opens a new session")
{
    FakeTransport fake;
    Client client(fake, settings());
    connectOk(fake, client);
    const auto &offered = fake.publications[static_cast<std::size_t>(fake.publicationTo("10.0.0.1:9302"))].offered;
    client.close();
    CHECK(templateOf(offered.back()) == cc::SessionCloseRequest::sbeTemplateId());
    CHECK(client.state() == ClientState::CLOSED);
    CHECK(client.closeReason() == CloseReason::CLIENT_CLOSE);
    CHECK_FALSE(fake.subscriptionOpen);
    client.close(); // idempotent

    REQUIRE(client.connect());
    drive(client);
    fake.pushSessionEvent(fake.lastConnectCorrelation(), 99, 7, 0, cc::EventCode::OK, "", 960);
    drive(client);
    CHECK(client.state() == ClientState::CONNECTED);
    CHECK(client.clusterSessionId() == 99);
    CHECK(client.sessionEpoch() == 2);
}

TEST_CASE("a handler that closes the client inside poll is deferred safely")
{
    FakeTransport fake;
    Client client(fake, settings());
    connectOk(fake, client);
    fake.pushAppMessage(77, { 1 }, 900);
    fake.pushAppMessage(77, { 2 }, 900);
    int delivered = 0;
    client.poll([&](const EgressMessage &)
        {
            delivered++;
            client.close();
        }, 10);
    CHECK(client.state() == ClientState::CLOSED);
    CHECK_FALSE(fake.closedSubscriptionInsidePoll);
}

TEST_CASE("connect() while active is refused, not restarted")
{
    FakeTransport fake;
    Client client(fake, settings());
    connectOk(fake, client);
    CHECK_FALSE(client.connect());
    CHECK(client.state() == ClientState::CONNECTED);
}

// ---- added by the S11 independent review (notebook .../RC2-me-link/S11/review.md) ----

namespace
{

// FakeTransport plus a per-image fragment assembler shaped like the Aeron C one
// (aeron_fragment_assembler.c: one builder per image session id, BEGIN starts it, a fragment
// without a builder is dropped, END delivers) and a resetEgressAssembly() that, like
// AeronTransport::makeAssembler(), throws every partial message away.
struct AssemblingTransport : FakeTransport
{
    struct Raw
    {
        std::vector<std::uint8_t> bytes;
        std::int32_t image = 0;
        int flags = 3; // 3 unfragmented, 2 BEGIN, 1 END, 0 middle
    };
    std::deque<Raw> raw;
    std::map<std::int32_t, std::vector<std::uint8_t>> partial;
    int droppedFragments = 0;

    void flush()
    {
        while (!egress.empty())
        {
            raw.push_back(Raw{ std::move(egress.front().bytes), egress.front().imageSessionId, 3 });
            egress.pop_front();
        }
    }

    // One app message for `sessionId` split into BEGIN + END fragments at `split`.
    void pushFragmentedApp(std::int64_t sessionId, const std::vector<std::uint8_t> &payload, std::int32_t image,
        std::size_t split)
    {
        pushAppMessage(sessionId, payload, image);
        std::vector<std::uint8_t> whole = std::move(egress.back().bytes);
        egress.pop_back();
        flush();
        raw.push_back(Raw{ std::vector<std::uint8_t>(whole.begin(), whole.begin() + split), image, 2 });
        raw.push_back(Raw{ std::vector<std::uint8_t>(whole.begin() + split, whole.end()), image, 1 });
    }

    template<typename Handler>
    int pollEgress(Handler &&handler, int fragmentLimit)
    {
        flush();
        pollCalls++;
        int count = 0;
        inPoll = true;
        while (subscriptionOpen && count < fragmentLimit && !raw.empty())
        {
            Raw fragment = std::move(raw.front());
            raw.pop_front();
            count++;
            if (3 == fragment.flags)
            {
                handler(EgressFragment{ fragment.bytes.data(), fragment.bytes.size(), fragment.image });
                continue;
            }
            if (0 != (fragment.flags & 2))
            {
                partial[fragment.image] = std::move(fragment.bytes);
                continue;
            }
            auto it = partial.find(fragment.image);
            if (it == partial.end())
            {
                droppedFragments++;
                continue;
            }
            it->second.insert(it->second.end(), fragment.bytes.begin(), fragment.bytes.end());
            if (0 != (fragment.flags & 1))
            {
                std::vector<std::uint8_t> message = std::move(it->second);
                partial.erase(it);
                handler(EgressFragment{ message.data(), message.size(), fragment.image });
            }
        }
        inPoll = false;
        return count;
    }

    void resetEgressAssembly()
    {
        assemblyResets++;
        partial.clear();
    }
};

static_assert(ClusterTransport<AssemblingTransport>);

}

TEST_CASE("review: a new-leader message whose fragments straddle two polls right after NewLeaderEvent is delivered")
{
    // Review D1 (was RED): AeronCluster clears its assembler INSIDE onNewLeader, so fragments
    // after the event in the same poll assemble normally. The client now resets at the event.
    AssemblingTransport fake;
    ClusterClient<AssemblingTransport> client(fake, settings());
    REQUIRE(client.connect());
    for (int i = 0; i < 20; i++)
    {
        client.doWork();
    }
    fake.pushSessionEvent(fake.lastConnectCorrelation(), 77, 5, 0, cc::EventCode::OK, "", 900);
    for (int i = 0; i < 20; i++)
    {
        client.doWork();
    }
    REQUIRE(client.isConnected());

    fake.pushNewLeader(77, 6, 2, DETAIL_ENDPOINTS, 950);
    fake.pushFragmentedApp(77, std::vector<std::uint8_t>(100, 0x5A), 950, 40);
    int delivered = 0;
    auto handler = [&](const EgressMessage &m) { delivered += (100 == m.length) ? 1 : 0; };
    client.poll(handler, 2); // NewLeaderEvent + BEGIN
    REQUIRE(client.state() == ClientState::AWAIT_NEW_LEADER_CONNECTION);
    client.poll(handler, 2); // END
    CHECK(fake.droppedFragments == 0);
    CHECK(delivered == 1);
}

TEST_CASE("review: the new leader's ingress registration failing closes the session at once (TRANSPORT_ERROR)")
{
    FakeTransport fake;
    Client client(fake, settings());
    connectOk(fake, client);
    fake.autoReady = false;
    fake.pushNewLeader(77, 6, 2, DETAIL_ENDPOINTS, 950);
    client.poll([](const EgressMessage &) {}, 10);
    REQUIRE(client.state() == ClientState::AWAIT_NEW_LEADER_CONNECTION);
    const PublicationId newPub = fake.publicationTo("10.0.0.3:9302");
    REQUIRE(newPub != NO_PUBLICATION);
    fake.publications[static_cast<std::size_t>(newPub)].status = Registration::FAILED;
    client.doWork();
    CHECK(client.state() == ClientState::CLOSED);
    CHECK(client.closeReason() == CloseReason::TRANSPORT_ERROR);
    CHECK_FALSE(fake.subscriptionOpen);
}

TEST_CASE("review: keepalive under sustained back-pressure is retried at the retry interval, each call returns at once")
{
    FakeTransport fake;
    Client client(fake, settings());
    connectOk(fake, client);
    auto &pub = fake.publications[static_cast<std::size_t>(fake.publicationTo("10.0.0.1:9302"))];
    pub.scriptedResults.assign(1000, OFFER_BACK_PRESSURED);
    fake.now += 1000 * MS;
    const int before = fake.offerCalls;
    for (int i = 0; i < 100; i++) // 100 ms of 1 ms duty cycles
    {
        client.doWork();
        fake.now += 1 * MS;
    }
    // One attempt per 10 ms retry interval, never a busy loop inside doWork.
    CHECK(fake.offerCalls - before == 10);
    CHECK(client.state() == ClientState::CONNECTED);
    pub.scriptedResults.clear();
    client.doWork();
    fake.now += 10 * MS;
    client.doWork();
    CHECK(client.counters().keepAlivesSent == 1);
}

TEST_CASE("F2: an absurd leaderHeartbeatTimeoutNs from the wire falls back to the default (no overflow)")
{
    FakeTransport fake;
    Client client(fake, settings());
    REQUIRE(client.connect());
    drive(client);
    fake.pushSessionEvent(fake.lastConnectCorrelation(), 77, 5, 0, cc::EventCode::OK, "", 900, INT64_MAX);
    drive(client);
    REQUIRE(client.isConnected());
    CHECK(client.leaderHeartbeatTimeoutNs() == LEADER_HEARTBEAT_TIMEOUT_DEFAULT_NS);
    CHECK(client.newLeaderTimeoutNs() == 2 * LEADER_HEARTBEAT_TIMEOUT_DEFAULT_NS);
}
