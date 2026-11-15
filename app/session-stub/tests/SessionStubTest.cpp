// ct_session_stub's pure parts: the strict command line, the stdin script, and the duty cycle over the FakeTransport
// (registration with the given mask and class, the poll throttle with doWork and keepalives running, reconnect and
// re-registration after the cluster closed the session, REJECTED as exit 78).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <algorithm>
#include <string>
#include <vector>

#include "FakeTransport.h"
#include "cluster_tools/client/ClusterClient.h"
#include "cluster_tools/runtime/LogRing.h"
#include "session-stub/SessionStub.h"
#include "session-stub/StubCommands.h"
#include "session-stub/StubOptions.h"

#include "com_openexchange_core_infrastructure_generated/AdminAck.h"
#include "com_openexchange_core_infrastructure_generated/MessageHeader.h"
#include "com_openexchange_core_infrastructure_generated/RegisterSession.h"

using namespace cluster_tools::app::session_stub;
using cluster_tools::client::test::FakeTransport;
namespace cl = cluster_tools::client;
namespace rt = cluster_tools::runtime;
namespace cc = io::aeron::cluster::codecs;
namespace money = com::openexchange::core::infrastructure::generated;

namespace
{

constexpr std::int64_t MS = 1'000'000LL;
using Client = cl::ClusterClient<FakeTransport>;
using Stub = SessionStub<Client>;

std::vector<std::string> validArgs()
{
    return { "--driver-mode", "external", "--aeron-dir", "/dev/shm/aeron", "--instance-id", "41",
        "--ingress-endpoints", "0=10.0.0.1:9302,1=10.0.0.2:9302", "--ingress-channel", "aeron:udp?term-length=4m|mtu=8192",
        "--response-channel", "aeron:udp?endpoint=10.0.0.9:0|term-length=4m|mtu=8192", "--channels", "acks,orders,control" };
}

std::vector<std::string> without(std::vector<std::string> args, const std::string &key)
{
    auto it = std::find(args.begin(), args.end(), key);
    REQUIRE(it != args.end());
    args.erase(it, it + 2);
    return args;
}

std::vector<std::string> with(std::vector<std::string> args, const std::string &key, const std::string &value)
{
    auto it = std::find(args.begin(), args.end(), key);
    if (it == args.end())
    {
        args.push_back(key);
        args.push_back(value);
    }
    else
    {
        *(it + 1) = value;
    }
    return args;
}

bool mentions(const OptionsResult &r, const std::string &text)
{
    return std::any_of(r.errors.begin(), r.errors.end(), [&](const std::string &e) { return e.find(text) != std::string::npos; });
}

StubOptions options(std::int64_t pollIntervalMs = 0)
{
    OptionsResult r = parseStubOptions(validArgs());
    REQUIRE(r.ok());
    r.options.pollIntervalMs = pollIntervalMs;
    r.options.statusIntervalMs = 0;
    r.options.reconnectDelayMs = 100;
    return r.options;
}

cl::ClientSettings settings()
{
    cl::ClientSettings s;
    s.ingressEndpoints = "0=10.0.0.1:9302,1=10.0.0.2:9302";
    s.responseChannel = "aeron:udp?endpoint=127.0.0.1:0";
    s.credentials = std::vector<std::uint8_t>(38, 1);
    return s;
}

std::vector<std::vector<std::uint8_t>> moneyFrames(const FakeTransport &fake, std::uint16_t templateId)
{
    std::vector<std::vector<std::uint8_t>> out;
    for (const auto &p : fake.publications)
    {
        for (const auto &frame : p.offered)
        {
            if (frame.size() > 40)
            {
                money::MessageHeader h(const_cast<char *>(reinterpret_cast<const char *>(frame.data())), 32, frame.size(), 0);
                if (h.schemaId() == 2 && h.templateId() == templateId)
                {
                    out.push_back(frame);
                }
            }
        }
    }
    return out;
}

std::size_t keepalives(const FakeTransport &fake)
{
    std::size_t n = 0;
    for (const auto &p : fake.publications)
    {
        for (const auto &frame : p.offered)
        {
            cc::MessageHeader h(const_cast<char *>(reinterpret_cast<const char *>(frame.data())), 0, frame.size(), 0);
            n += h.schemaId() == cc::MessageHeader::sbeSchemaId() && h.templateId() == cc::SessionKeepAlive::sbeTemplateId();
        }
    }
    return n;
}

std::vector<std::uint8_t> adminAck(std::int64_t correlationId, money::AdminResult::Value result)
{
    std::vector<std::uint8_t> bytes(64);
    money::AdminAck ack;
    ack.wrapAndApplyHeader(reinterpret_cast<char *>(bytes.data()), 0, bytes.size())
        .correlationId(correlationId)
        .refTemplateId(80)
        .result(result)
        .reason(3)
        .configVersion(0);
    bytes.resize(money::MessageHeader::encodedLength() + ack.encodedLength());
    return bytes;
}

// One duty cycle per ms of the fake clock.
void run(FakeTransport &fake, Stub &stub, std::int64_t ms)
{
    for (std::int64_t i = 0; i < ms; i++)
    {
        fake.now += MS;
        stub.dutyCycle();
    }
}

void connect(FakeTransport &fake, Stub &stub, std::int64_t sessionId = 77)
{
    REQUIRE(stub.start());
    run(fake, stub, 20);
    REQUIRE(fake.lastConnectCorrelation() >= 0);
    fake.pushSessionEvent(fake.lastConnectCorrelation(), sessionId, 5, 0, cc::EventCode::OK, "", 900);
    run(fake, stub, 5);
    REQUIRE(stub.counters().dutyCycles > 0);
}

std::vector<std::string> lines(rt::LogRing &ring)
{
    std::vector<std::string> out;
    ring.drain([&](const rt::LogRecord &r) { out.emplace_back(r.text); });
    return out;
}

bool hasLine(const std::vector<std::string> &all, const std::string &text)
{
    return std::any_of(all.begin(), all.end(), [&](const std::string &l) { return l.find(text) != std::string::npos; });
}

void submit(Stub &stub, const std::string &line)
{
    CommandResult r = parseCommand(line);
    REQUIRE_MESSAGE(r.ok(), r.error);
    stub.submit(r.command);
}

}

TEST_CASE("options: a full set parses; every missing required key is named in one pass")
{
    const OptionsResult ok = parseStubOptions(validArgs());
    REQUIRE(ok.ok());
    CHECK(ok.options.driverMode == DriverMode::EXTERNAL);
    CHECK(ok.options.instanceId == 41);
    CHECK(ok.options.channels == (cl::core_channel::ACKS | cl::core_channel::ORDERS | cl::core_channel::CONTROL));
    CHECK(ok.options.pollIntervalMs == 0);
    CHECK(ok.options.pollFragments == 10);
    CHECK(ok.options.credentialEnv == "CORE_AUTH_CREDENTIAL");
    CHECK(ok.options.reconnect);

    const OptionsResult none = parseStubOptions({});
    for (const char *key : { "--driver-mode", "--instance-id", "--ingress-endpoints", "--ingress-channel",
             "--response-channel", "--channels" })
    {
        CHECK_MESSAGE(mentions(none, std::string(key) + " is required"), key);
    }
}

TEST_CASE("options: malformed values are refused, never defaulted")
{
    CHECK(mentions(parseStubOptions(with(validArgs(), "--instance-id", "0")), "--instance-id must be a positive int64"));
    CHECK(mentions(parseStubOptions(with(validArgs(), "--instance-id", "x7")), "--instance-id must be a positive int64"));
    CHECK(mentions(parseStubOptions(with(validArgs(), "--driver-mode", "Embedded")), "exactly embedded or external"));
    CHECK(mentions(parseStubOptions(with(validArgs(), "--ingress-channel", "aeron:udp?term-length=4m")),
        "--ingress-channel must name mtu explicitly"));
    CHECK(mentions(parseStubOptions(with(validArgs(), "--response-channel", "aeron:udp?term-length=4m|mtu=8192")),
        "--response-channel must name endpoint explicitly"));
    CHECK(mentions(parseStubOptions(with(validArgs(), "--driver-dir", "/dev/shm")), "--driver-dir is not a setting of driver mode external"));
    CHECK(mentions(parseStubOptions(without(validArgs(), "--aeron-dir")), "--aeron-dir is required"));
    CHECK(mentions(parseStubOptions(with(with(validArgs(), "--driver-mode", "embedded"), "--driver-dir", "/dev/shm")),
        "--aeron-dir is not a setting of driver mode embedded"));
    CHECK(mentions(parseStubOptions(with(validArgs(), "--driver-timeout-ms", "100")), "--driver-timeout-ms must be an integer in"));
    CHECK(mentions(parseStubOptions(with(validArgs(), "--poll-fragments", "0")), "--poll-fragments must be a positive integer"));
    CHECK(mentions(parseStubOptions(with(validArgs(), "--reconnect", "yes")), "--reconnect must be on or off"));
    CHECK(mentions(parseStubOptions(with(validArgs(), "--idle", "spin")), "--idle must be backoff or busy"));
    CHECK(mentions(parseStubOptions(with(validArgs(), "--bogus", "1")), "unknown option '--bogus'"));
    std::vector<std::string> twice = validArgs();
    twice.push_back("--channels");
    twice.push_back("acks");
    CHECK(mentions(parseStubOptions(twice), "--channels is given twice"));
    // settings, not limits: a large poll interval and fragment limit are accepted
    const OptionsResult big = parseStubOptions(with(with(validArgs(), "--poll-interval-ms", "600000"), "--poll-fragments", "100000"));
    CHECK(big.ok());
}

TEST_CASE("channel mask: names, decimal, hex; unknown names and bits are refused")
{
    std::uint32_t mask = 0;
    std::string error;
    CHECK(parseChannelMask("acks,balances,orders,control", mask, error));
    CHECK(mask == (cl::core_channel::ACKS | cl::core_channel::BALANCES | cl::core_channel::ORDERS | cl::core_channel::CONTROL));
    CHECK(channelMaskNames(mask) == "acks,balances,orders,control");
    CHECK(parseChannelMask("0x51", mask, error));
    CHECK(mask == 0x51);
    CHECK(parseChannelMask("127", mask, error));
    CHECK(mask == 127);
    CHECK_FALSE(parseChannelMask("128", mask, error));
    CHECK(error.find("unknown channel bits") != std::string::npos);
    CHECK_FALSE(parseChannelMask("acks,trades", mask, error));
    CHECK(error.find("unknown channel 'trades'") != std::string::npos);
    CHECK_FALSE(parseChannelMask("acks,", mask, error));
}

TEST_CASE("commands: every verb parses; malformed lines are named")
{
    CHECK(parseCommand("").command.kind == CommandKind::NONE);
    CHECK(parseCommand("  # a comment").command.kind == CommandKind::NONE);
    CHECK(parseCommand("throttle off").command.kind == CommandKind::THROTTLE_OFF);
    CHECK(parseCommand("throttle pause").command.kind == CommandKind::THROTTLE_PAUSE);
    const CommandResult t = parseCommand("throttle 250 3");
    CHECK(t.command.kind == CommandKind::THROTTLE);
    CHECK(t.command.valueMs == 250);
    CHECK(t.command.fragments == 3);
    const CommandResult o = parseCommand("offer 0a0BfF");
    REQUIRE(o.ok());
    CHECK(o.command.bytes == std::vector<std::uint8_t>{ 0x0a, 0x0b, 0xff });
    CHECK(parseCommand("await registered 5000").command.kind == CommandKind::AWAIT_REGISTERED);
    CHECK(parseCommand("await closed 0").command.kind == CommandKind::AWAIT_CLOSED);
    CHECK(parseCommand("sleep 10").command.kind == CommandKind::SLEEP);
    CHECK(parseCommand("reconnect off").command.kind == CommandKind::RECONNECT);
    CHECK_FALSE(parseCommand("reconnect off").command.flag);
    CHECK(parseCommand("close").command.kind == CommandKind::CLOSE);
    CHECK(parseCommand("quit").command.kind == CommandKind::QUIT);
    CHECK(parseCommand("status").command.kind == CommandKind::STATUS);

    CHECK_FALSE(parseCommand("throttle 0").ok());
    CHECK_FALSE(parseCommand("throttle -5").ok());
    CHECK_FALSE(parseCommand("throttle 10 0").ok());
    CHECK_FALSE(parseCommand("offer abc").ok());
    CHECK_FALSE(parseCommand("offer zz").ok());
    CHECK_FALSE(parseCommand("await opened 10").ok());
    CHECK_FALSE(parseCommand("sleep").ok());
    CHECK(parseCommand("frobnicate").error.find("unknown command 'frobnicate'") != std::string::npos);
}

TEST_CASE("registers one GATEWAY session with the mask and identity of the command line")
{
    FakeTransport fake;
    Client client(fake, settings());
    rt::LogRing out;
    Stub stub(client, options(), out);
    REQUIRE(stub.validate().empty());
    connect(fake, stub);
    REQUIRE(client.isConnected());

    const auto frames = moneyFrames(fake, 80);
    REQUIRE(frames.size() == 1);
    char *p = const_cast<char *>(reinterpret_cast<const char *>(frames[0].data()));
    money::MessageHeader h(p, 32, frames[0].size(), 0);
    money::RegisterSession r(p, 40, frames[0].size(), h.blockLength(), h.version());
    CHECK(r.sessionClass() == money::SessionClass::GATEWAY);
    CHECK(r.channels().acks());
    CHECK(r.channels().orders());
    CHECK(r.channels().control());
    CHECK_FALSE(r.channels().balances());
    CHECK_FALSE(r.channels().dispatch());
    CHECK(r.identity() == 41);
    CHECK(r.resumable() == money::BoolFlag::TRUE);

    fake.pushAppMessage(77, adminAck(stub.registrar().correlationId(), money::AdminResult::ACCEPTED), 900);
    run(fake, stub, 2);
    CHECK(stub.registrar().isRegistered());
    const auto all = lines(out);
    CHECK(hasLine(all, "event=start channels=acks,orders,control mask=0x51"));
    CHECK(hasLine(all, "event=registration state=REGISTERED"));
}

TEST_CASE("throttle: poll at most once per interval while doWork and the keepalives keep running")
{
    FakeTransport fake;
    Client client(fake, settings());
    rt::LogRing out;
    Stub stub(client, options(), out);
    connect(fake, stub);
    fake.pushAppMessage(77, adminAck(stub.registrar().correlationId(), money::AdminResult::ACCEPTED), 900);
    run(fake, stub, 2);
    REQUIRE(stub.registrar().isRegistered());

    submit(stub, "throttle 100 1");
    run(fake, stub, 1);
    for (int i = 0; i < 400; i++)
    {
        fake.pushAppMessage(77, std::vector<std::uint8_t>(40, 0), 900); // a backlog the stub drains slowly
    }
    const int pollsBefore = fake.pollCalls;
    const std::size_t keepalivesBefore = keepalives(fake);
    run(fake, stub, 3'000);
    const int polls = fake.pollCalls - pollsBefore;
    CHECK_MESSAGE(polls <= 31, "poll ran " << polls << " times in 3 s: the throttle is not applied");
    CHECK(polls >= 29);
    CHECK_MESSAGE(fake.egress.size() >= 400 - 31, "the backlog drained faster than one fragment per interval");
    CHECK_MESSAGE(keepalives(fake) >= keepalivesBefore + 2, "keepalives stopped under the throttle");
    CHECK(client.isConnected());

    submit(stub, "throttle pause");
    run(fake, stub, 1);
    const int pausedPolls = fake.pollCalls;
    const std::size_t pausedKeepalives = keepalives(fake);
    const std::size_t backlog = fake.egress.size();
    run(fake, stub, 3'000);
    CHECK_MESSAGE(fake.pollCalls == pausedPolls, "poll ran while paused");
    CHECK(fake.egress.size() == backlog);
    CHECK_MESSAGE(keepalives(fake) >= pausedKeepalives + 2, "keepalives stopped under the throttle");
    CHECK(client.isConnected());

    submit(stub, "throttle off");
    run(fake, stub, 100);
    CHECK(fake.egress.empty());
    CHECK(stub.counters().egressMessages >= 400);
    const auto all = lines(out);
    CHECK(hasLine(all, "event=throttle mode=interval interval_ms=100 fragments=1"));
    CHECK(hasLine(all, "event=throttle mode=pause"));
    CHECK(hasLine(all, "event=throttle mode=off"));
}

TEST_CASE("a session closed by the cluster is replaced after the delay and registers again")
{
    FakeTransport fake;
    Client client(fake, settings());
    rt::LogRing out;
    Stub stub(client, options(), out);
    connect(fake, stub);
    fake.pushAppMessage(77, adminAck(stub.registrar().correlationId(), money::AdminResult::ACCEPTED), 900);
    run(fake, stub, 2);
    REQUIRE(stub.registrar().isRegistered());

    submit(stub, "await closed 10000");
    run(fake, stub, 1); // the script reaches the await: a close from now on counts
    fake.pushSessionEvent(0, 77, 5, 0, cc::EventCode::CLOSED, "session timed out", 900);
    run(fake, stub, 5);
    CHECK(client.state() == cl::ClientState::CLOSED);
    CHECK(stub.counters().reconnects == 0); // not before the delay
    run(fake, stub, 120);
    CHECK(stub.counters().reconnects == 1);
    run(fake, stub, 20);
    fake.pushSessionEvent(fake.lastConnectCorrelation(), 78, 5, 0, cc::EventCode::OK, "", 901);
    run(fake, stub, 5);
    REQUIRE(client.isConnected());
    CHECK(client.sessionEpoch() == 2);
    CHECK(moneyFrames(fake, 80).size() == 2);
    const auto all = lines(out);
    CHECK(hasLine(all, "close_reason=SESSION_CLOSED_BY_CLUSTER"));
    CHECK(hasLine(all, "event=await what=closed result=ok"));
    CHECK(hasLine(all, "event=reconnect started=true"));
}

TEST_CASE("reconnect off: a closed session stays closed")
{
    FakeTransport fake;
    Client client(fake, settings());
    rt::LogRing out;
    Stub stub(client, options(), out);
    connect(fake, stub);
    submit(stub, "reconnect off");
    run(fake, stub, 1);
    fake.pushSessionEvent(0, 77, 5, 0, cc::EventCode::CLOSED, "session timed out", 900);
    run(fake, stub, 500);
    CHECK(client.state() == cl::ClientState::CLOSED);
    CHECK(stub.counters().reconnects == 0);
}

TEST_CASE("script: offer passes the bytes through, sleep and await hold the script, quit ends with 0")
{
    FakeTransport fake;
    Client client(fake, settings());
    rt::LogRing out;
    Stub stub(client, options(), out);
    connect(fake, stub);

    submit(stub, "await registered 50");
    submit(stub, "sleep 100");
    submit(stub, "offer 1800370002000700deadbeef");
    submit(stub, "quit");
    run(fake, stub, 40);
    CHECK(stub.counters().offers == 0); // held by the await
    run(fake, stub, 20);                // the await times out (no ack), the sleep starts
    CHECK(stub.counters().offers == 0);
    run(fake, stub, 110);
    CHECK(stub.counters().offers == 1);
    CHECK(stub.counters().offersOk == 1);
    CHECK(stub.finished());
    CHECK(stub.exitCode() == 0);
    bool found = false;
    for (const auto &p : fake.publications)
    {
        for (const auto &frame : p.offered)
        {
            found = found || (frame.size() == 32 + 12 && frame[32] == 0x18 && frame[43] == 0xef);
        }
    }
    CHECK_MESSAGE(found, "the offered bytes did not reach the transport after the session header");
    const auto all = lines(out);
    CHECK(hasLine(all, "event=await what=registered result=timeout"));
    CHECK(hasLine(all, "event=offer result=OK bytes=12"));
    CHECK(hasLine(all, "event=quit"));
}

TEST_CASE("a bad script line is reported and skipped")
{
    FakeTransport fake;
    Client client(fake, settings());
    rt::LogRing out;
    Stub stub(client, options(), out);
    const CommandResult r = parseCommand("throttle sideways");
    REQUIRE_FALSE(r.ok());
    stub.reportCommandError(r.command.text, r.error);
    CHECK(stub.counters().commandErrors == 1);
    CHECK(hasLine(lines(out), "event=command_error line=\"throttle sideways\""));

    // in a script, a line that did not parse is reported in its place: after the commands before it
    CommandResult bad = parseCommand("sleep soon");
    REQUIRE_FALSE(bad.ok());
    bad.command.kind = CommandKind::INVALID;
    bad.command.error = bad.error;
    submit(stub, "throttle pause");
    stub.submit(bad.command);
    submit(stub, "throttle off");
    run(fake, stub, 1);
    const auto all = lines(out);
    std::vector<std::string> events;
    for (const auto &l : all)
    {
        if (l.rfind("event=throttle", 0) == 0 || l.rfind("event=command_error", 0) == 0)
        {
            events.push_back(l.substr(0, 40));
        }
    }
    REQUIRE(events.size() == 3);
    CHECK(events[0].rfind("event=throttle mode=pause", 0) == 0);
    CHECK(events[1].rfind("event=command_error line=\"sleep soon", 0) == 0);
    CHECK(events[2].rfind("event=throttle mode=off", 0) == 0);
    CHECK(stub.counters().commandErrors == 2);
}

TEST_CASE("a REJECTED registration ends the stub with 78")
{
    FakeTransport fake;
    Client client(fake, settings());
    rt::LogRing out;
    Stub stub(client, options(), out);
    connect(fake, stub);
    fake.pushAppMessage(77, adminAck(stub.registrar().correlationId(), money::AdminResult::REJECTED), 900);
    run(fake, stub, 2);
    CHECK(stub.finished());
    CHECK(stub.exitCode() == 78);
    CHECK(hasLine(lines(out), "event=registration_rejected"));
}
