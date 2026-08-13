#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "cluster_tools/ports/ClusterSession.h"
#include "cluster_tools/ports/Result.h"

// These tests run in the WITH_AERON=OFF build: they compile-prove the port headers and
// exercise Result with zero infrastructure. The fake below is the hexagon's receipt —
// the ClusterSession seam is implementable without a single aeron header in sight.

using cluster_tools::ports::ClusterSession;
using cluster_tools::ports::EgressHandler;
using cluster_tools::ports::EgressMessage;
using cluster_tools::ports::Result;
using cluster_tools::ports::SessionInfo;

namespace
{

class FakeClusterSession final : public ClusterSession
{
public:
    Result<SessionInfo> connect(const std::string &ingressEndpoints) override
    {
        if (ingressEndpoints.empty())
        {
            return Result<SessionInfo>::failure("ingress endpoints required");
        }
        SessionInfo info;
        info.sessionId = 7;
        info.leaderMemberId = 2;
        info.leadershipTermId = 41;
        m_connected = true;
        return Result<SessionInfo>::success(info);
    }

    Result<std::int64_t> offer(const std::uint8_t *, std::size_t length) override
    {
        if (!m_connected)
        {
            return Result<std::int64_t>::failure("not connected");
        }
        m_position += static_cast<std::int64_t>(length);
        return Result<std::int64_t>::success(m_position);
    }

    Result<int> poll(const EgressHandler &handler, int) override
    {
        if (!m_connected)
        {
            return Result<int>::failure("not connected");
        }
        const std::uint8_t payload[] = { 0x08, 0x00, 0x15, 0x00, 0x01, 0x00, 0x09, 0x00 };
        EgressMessage message;
        message.clusterSessionId = 7;
        message.payload = payload;
        message.length = sizeof(payload);
        handler(message);
        return Result<int>::success(1);
    }

    void close() override
    {
        m_connected = false;
    }

private:
    bool m_connected = false;
    std::int64_t m_position = 0;
};

}

TEST_CASE("Result carries a value on success and a reason on failure")
{
    const auto good = Result<int>::success(42);
    CHECK(good.ok());
    CHECK(good.value() == 42);
    CHECK(good.error().empty());

    const auto bad = Result<int>::failure("no route to leader");
    CHECK_FALSE(bad.ok());
    CHECK(bad.error() == "no route to leader");
}

TEST_CASE("Result works with struct payloads")
{
    SessionInfo info;
    info.sessionId = 99;
    const auto result = Result<SessionInfo>::success(info);
    CHECK(result.ok());
    CHECK(result.value().sessionId == 99);
    CHECK(result.value().leaderMemberId == -1);
}

TEST_CASE("the ClusterSession port is implementable with zero infrastructure")
{
    // Held through the interface on purpose: this is how app code will hold it.
    std::unique_ptr<ClusterSession> session = std::make_unique<FakeClusterSession>();

    const auto refused = session->connect("");
    CHECK_FALSE(refused.ok());

    const auto connected = session->connect("0=10.0.0.1:9002,1=10.0.0.2:9002");
    REQUIRE(connected.ok());
    CHECK(connected.value().sessionId == 7);
    CHECK(connected.value().leaderMemberId == 2);
    CHECK(connected.value().leadershipTermId == 41);

    const std::uint8_t frame[16] = { 0 };
    const auto offered = session->offer(frame, sizeof(frame));
    REQUIRE(offered.ok());
    CHECK(offered.value() == 16);

    std::vector<EgressMessage> seen;
    std::vector<std::uint8_t> copied;
    const auto polled = session->poll(
        [&](const EgressMessage &message)
        {
            seen.push_back(message);
            copied.assign(message.payload, message.payload + message.length);
        },
        10);
    REQUIRE(polled.ok());
    CHECK(polled.value() == 1);
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].clusterSessionId == 7);
    CHECK(copied.size() == 8);

    session->close();
    CHECK_FALSE(session->offer(frame, sizeof(frame)).ok());
}
