// poc.cpp — proof-of-concept C++ Aeron Cluster client against a 3-node match cluster.
//
// Protocol mirrored from io.aeron.cluster.client.AeronCluster (aeron 1.52.2):
//   ingress streamId 101, egress streamId 102, PROTOCOL_SEMANTIC_VERSION 0.3.0 = 768.
//   Connect: SessionConnectRequest on any member's ingress; follower answers
//   SessionEvent code=REDIRECT (leaderMemberId), leader answers code=OK with
//   clusterSessionId + leadershipTermId. Every app ingress message is prefixed with
//   MessageHeader(schema 111) + SessionMessageHeader{leadershipTermId, clusterSessionId,
//   timestamp=0} (timestamp left 0, exactly like the Java client's headerBuffer).
//
// App payload: match order-schema (schemaId=1, version=9) CreateOrder (templateId 1).
// Egress app payloads arrive SessionMessageHeader-framed; OrderStatusBatch (24) carries
// the ack/reject for our omsOrderId.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

#include <Aeron.h>
#include <FragmentAssembler.h>

#include "io_aeron_cluster_codecs/MessageHeader.h"
#include "io_aeron_cluster_codecs/VarAsciiEncoding.h"
#include "io_aeron_cluster_codecs/EventCode.h"
#include "io_aeron_cluster_codecs/SessionConnectRequest.h"
#include "io_aeron_cluster_codecs/SessionEvent.h"
#include "io_aeron_cluster_codecs/SessionMessageHeader.h"
#include "io_aeron_cluster_codecs/SessionKeepAlive.h"
#include "io_aeron_cluster_codecs/NewLeaderEvent.h"

#include "com_match_infrastructure_generated/MessageHeader.h"
#include "com_match_infrastructure_generated/GroupSizeEncoding.h"
#include "com_match_infrastructure_generated/OrderType.h"
#include "com_match_infrastructure_generated/OrderSide.h"
#include "com_match_infrastructure_generated/OrderStatus.h"
#include "com_match_infrastructure_generated/BookUpdateType.h"
#include "com_match_infrastructure_generated/CreateOrder.h"
#include "com_match_infrastructure_generated/OrderStatusBatch.h"
#include "com_match_infrastructure_generated/TradesBatch.h"
#include "com_match_infrastructure_generated/TradeExecutionBatch.h"
#include "com_match_infrastructure_generated/ClusterHeartbeat.h"

namespace cc = io::aeron::cluster::codecs;
namespace mm = com::match::infrastructure::generated;

using namespace aeron;
using namespace aeron::concurrent;

namespace
{
constexpr std::int32_t INGRESS_STREAM_ID = 101;              // AeronCluster.Configuration.INGRESS_STREAM_ID_DEFAULT
constexpr std::int32_t EGRESS_STREAM_ID = 102;               // AeronCluster.Configuration.EGRESS_STREAM_ID_DEFAULT
constexpr std::int32_t PROTOCOL_SEMANTIC_VERSION = (0 << 16) | (3 << 8) | 0; // SemanticVersion.compose(0,3,0)

constexpr std::int64_t ORDER_USER_ID = 7;
constexpr std::int64_t ORDER_PRICE = 6'000'000'000'000LL;    // $60,000.00000000 — BTC band [50k,150k], tick $1
constexpr std::int64_t ORDER_QTY = 100'000'000LL;            // 1.00000000 BTC
constexpr std::int32_t ORDER_MARKET_ID = 1;                  // BTC-USD
constexpr std::int64_t ORDER_OMS_ORDER_ID = 424242;

std::int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string envOr(const char *name, const std::string &dflt)
{
    const char *v = ::getenv(name);
    return (nullptr != v && '\0' != *v) ? std::string(v) : dflt;
}

void hexDump(const char *label, const std::uint8_t *data, int length)
{
    const int n = length < 64 ? length : 64;
    std::printf("%s hex[0..%d]:", label, n - 1);
    for (int i = 0; i < n; i++)
    {
        std::printf("%s%02x", (i % 8 == 0) ? " " : "", data[i]);
    }
    std::printf("\n");
}

const char *orderStatusName(int v)
{
    switch (v)
    {
        case 0: return "NEW";
        case 1: return "PARTIALLY_FILLED";
        case 2: return "FILLED";
        case 3: return "CANCELLED";
        case 4: return "REJECTED";
        default: return "?";
    }
}

struct Member
{
    int id;
    std::string endpoint;
    std::shared_ptr<Publication> pub;
};

struct PocState
{
    // goal 1
    bool connected = false;
    std::int64_t clusterSessionId = -1;
    std::int64_t leadershipTermId = -1;
    std::int32_t leaderMemberId = -1;
    int connectedViaMember = -1;
    int redirectTo = -1;
    bool fatal = false;
    // goal 3
    bool anyAppEgress = false;
    bool orderResponseSeen = false;
    long egressCount = 0;
    std::map<std::int64_t, int> outstandingConnects; // correlationId -> member idx
};
} // namespace

int main()
{
    const std::string aeronDir = envOr("AERON_DIR", "/dev/shm/aeron-poc");
    const std::string selfIp = envOr("SELF_IP", "172.28.0.20");
    const std::string endpointsSpec = envOr(
        "INGRESS_ENDPOINTS", "0=172.28.0.10:9002,1=172.28.0.11:9002,2=172.28.0.12:9002");
    const std::string egressChannel = "aeron:udp?endpoint=" + selfIp + ":19999";

    std::printf("poc: aeronDir=%s egressChannel=%s ingress=%s\n",
        aeronDir.c_str(), egressChannel.c_str(), endpointsSpec.c_str());

    // --- parse "id=host:port,..." exactly like AeronCluster.parseIngressEndpoints ---
    std::vector<Member> members;
    {
        std::size_t pos = 0;
        while (pos < endpointsSpec.size())
        {
            std::size_t comma = endpointsSpec.find(',', pos);
            if (comma == std::string::npos)
            {
                comma = endpointsSpec.size();
            }
            const std::string tok = endpointsSpec.substr(pos, comma - pos);
            const std::size_t eq = tok.find('=');
            members.push_back(Member{ std::stoi(tok.substr(0, eq)), tok.substr(eq + 1), nullptr });
            pos = comma + 1;
        }
    }

    Context context;
    context.aeronDir(aeronDir);
    std::shared_ptr<Aeron> client = Aeron::connect(context);

    // --- egress subscription + one ingress publication per member ---
    const std::int64_t subRegId = client->addSubscription(egressChannel, EGRESS_STREAM_ID);
    std::vector<std::int64_t> pubRegIds;
    for (const Member &m : members)
    {
        pubRegIds.push_back(client->addPublication("aeron:udp?endpoint=" + m.endpoint, INGRESS_STREAM_ID));
    }

    std::shared_ptr<Subscription> egressSub;
    while (!(egressSub = client->findSubscription(subRegId)))
    {
        std::this_thread::yield();
    }
    for (std::size_t i = 0; i < members.size(); i++)
    {
        while (!(members[i].pub = client->findPublication(pubRegIds[i])))
        {
            std::this_thread::yield();
        }
    }

    PocState st;
    char sendBuf[512];

    // --- egress fragment handler: SessionEvent / NewLeaderEvent / SessionMessageHeader ---
    fragment_handler_t handler =
        [&](AtomicBuffer &buffer, util::index_t offset, util::index_t length, logbuffer::Header &)
    {
        char *buf = reinterpret_cast<char *>(buffer.buffer());
        const std::uint64_t cap = static_cast<std::uint64_t>(offset) + length;
        cc::MessageHeader hdr(buf, offset, cap, 0);
        const std::uint64_t bodyOffset = offset + hdr.encodedLength();

        if (hdr.schemaId() != cc::MessageHeader::sbeSchemaId())
        {
            std::printf("EGRESS unexpected schemaId=%d templateId=%d len=%d\n",
                (int)hdr.schemaId(), (int)hdr.templateId(), (int)length);
            return;
        }

        switch (hdr.templateId())
        {
            case 2: // SessionEvent
            {
                cc::SessionEvent ev(buf, bodyOffset, cap, hdr.blockLength(), hdr.version());
                const std::int64_t corr = ev.correlationId();
                const int code = (int)ev.code();
                std::string detail = ev.getDetailAsString();
                std::printf("SESSION_EVENT corr=%lld code=%d sessionId=%lld term=%lld leader=%d detail=\"%s\"\n",
                    (long long)corr, code, (long long)ev.clusterSessionId(),
                    (long long)ev.leadershipTermId(), (int)ev.leaderMemberId(), detail.c_str());

                auto it = st.outstandingConnects.find(corr);
                if (it != st.outstandingConnects.end())
                {
                    if (cc::EventCode::OK == ev.code())
                    {
                        st.connected = true;
                        st.clusterSessionId = ev.clusterSessionId();
                        st.leadershipTermId = ev.leadershipTermId();
                        st.leaderMemberId = ev.leaderMemberId();
                        st.connectedViaMember = it->second;
                    }
                    else if (cc::EventCode::REDIRECT == ev.code())
                    {
                        st.redirectTo = ev.leaderMemberId();
                    }
                    else if (cc::EventCode::ERROR == ev.code() ||
                             cc::EventCode::AUTHENTICATION_REJECTED == ev.code())
                    {
                        st.fatal = true;
                    }
                }
                else if (cc::EventCode::CLOSED == ev.code() &&
                         st.connected && ev.clusterSessionId() == st.clusterSessionId)
                {
                    std::printf("SESSION_EVENT: session CLOSED by cluster\n");
                    st.fatal = true;
                }
                break;
            }
            case 6: // NewLeaderEvent
            {
                cc::NewLeaderEvent ev(buf, bodyOffset, cap, hdr.blockLength(), hdr.version());
                std::printf("NEW_LEADER_EVENT term=%lld sessionId=%lld leader=%d endpoints=%s\n",
                    (long long)ev.leadershipTermId(), (long long)ev.clusterSessionId(),
                    (int)ev.leaderMemberId(), ev.getIngressEndpointsAsString().c_str());
                break;
            }
            case 1: // SessionMessageHeader — app payload follows
            {
                cc::SessionMessageHeader smh(buf, bodyOffset, cap, hdr.blockLength(), hdr.version());
                const std::uint64_t appOffset = bodyOffset + smh.encodedLength();
                mm::MessageHeader appHdr(buf, appOffset, cap, 0);
                const int appLen = (int)(cap - appOffset);
                st.egressCount++;

                std::printf("EGRESS templateId=%d schemaId=%d version=%d len=%d sessionId=%lld\n",
                    (int)appHdr.templateId(), (int)appHdr.schemaId(), (int)appHdr.version(),
                    appLen, (long long)smh.clusterSessionId());

                if (appHdr.schemaId() != mm::MessageHeader::sbeSchemaId())
                {
                    return;
                }
                st.anyAppEgress = true;
                const std::uint64_t appBody = appOffset + appHdr.encodedLength();

                switch (appHdr.templateId())
                {
                    case 24: // OrderStatusBatch
                    {
                        hexDump("  OrderStatusBatch",
                            reinterpret_cast<std::uint8_t *>(buf) + appOffset, appLen);
                        mm::OrderStatusBatch osb(buf, appBody, cap, appHdr.blockLength(), appHdr.version());
                        auto &orders = osb.orders();
                        while (orders.hasNext())
                        {
                            orders.next();
                            std::printf(
                                "  ORDER_STATUS orderId=%lld omsOrderId=%lld status=%s(%d) rejectReason=%d "
                                "price=%lld remainingQty=%lld statusSeq=%lld\n",
                                (long long)orders.orderId(), (long long)orders.omsOrderId(),
                                orderStatusName((int)orders.status()), (int)orders.status(),
                                (int)orders.rejectReason(), (long long)orders.price(),
                                (long long)orders.remainingQty(), (long long)orders.statusSeq());
                            if (orders.omsOrderId() == ORDER_OMS_ORDER_ID)
                            {
                                st.orderResponseSeen = true;
                            }
                        }
                        break;
                    }
                    case 21: // ClusterHeartbeat
                    {
                        mm::ClusterHeartbeat hb(buf, appBody, cap, appHdr.blockLength(), appHdr.version());
                        std::printf("  CLUSTER_HEARTBEAT nodeId=%d timestamp=%lld\n",
                            (int)hb.nodeId(), (long long)hb.timestamp());
                        break;
                    }
                    case 22: std::printf("  BOOK_SNAPSHOT\n"); break;
                    case 23: std::printf("  TRADES_BATCH\n"); break;
                    case 25: std::printf("  BOOK_DELTA\n"); break;
                    case 26: std::printf("  TRADE_EXECUTION_BATCH\n"); break;
                    case 27: std::printf("  OPEN_ORDERS_SNAPSHOT\n"); break;
                    default:
                        hexDump("  app", reinterpret_cast<std::uint8_t *>(buf) + appOffset, appLen);
                        break;
                }
                break;
            }
            default:
                std::printf("EGRESS cluster-proto templateId=%d len=%d\n", (int)hdr.templateId(), (int)length);
                break;
        }
    };

    FragmentAssembler assembler(handler);
    fragment_handler_t assembling = assembler.handler();

    // ---------------- goal 1: connect ----------------
    const std::int64_t connectDeadline = nowMs() + 60000;
    std::int64_t lastSend = 0;
    std::int64_t corrSeq = nowMs() * 1000;

    while (!st.connected)
    {
        if (st.fatal)
        {
            std::printf("FAIL: connect refused (ERROR/AUTHENTICATION_REJECTED)\n");
            return 1;
        }
        if (nowMs() > connectDeadline)
        {
            std::printf("FAIL: no SessionEvent OK within 60s\n");
            return 1;
        }

        if (nowMs() - lastSend > 600)
        {
            for (std::size_t i = 0; i < members.size(); i++)
            {
                if (st.redirectTo >= 0 && members[i].id != st.redirectTo)
                {
                    continue;
                }
                if (!members[i].pub->isConnected())
                {
                    continue;
                }
                const std::int64_t corr = ++corrSeq;
                cc::SessionConnectRequest req;
                req.wrapAndApplyHeader(sendBuf, 0, sizeof(sendBuf))
                    .correlationId(corr)
                    .responseStreamId(EGRESS_STREAM_ID)
                    .version(PROTOCOL_SEMANTIC_VERSION);
                req.putResponseChannel(egressChannel);
                req.putEncodedCredentials("", 0);
                req.putClientInfo(std::string("name=cpp-poc version=0.1"));
                const util::index_t len =
                    (util::index_t)(cc::MessageHeader::encodedLength() + req.encodedLength());

                AtomicBuffer ab(reinterpret_cast<std::uint8_t *>(sendBuf), (size_t)len);
                const std::int64_t result = members[i].pub->offer(ab, 0, len);
                std::printf("CONNECT_REQ member=%d endpoint=%s corr=%lld offer=%lld\n",
                    members[i].id, members[i].endpoint.c_str(), (long long)corr, (long long)result);
                if (result > 0)
                {
                    st.outstandingConnects[corr] = (int)i;
                }
            }
            lastSend = nowMs();
        }

        egressSub->poll(assembling, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::printf("CONNECTED sessionId=%lld leader=%d leadershipTermId=%lld via=member%d\n",
        (long long)st.clusterSessionId, (int)st.leaderMemberId,
        (long long)st.leadershipTermId, members[st.connectedViaMember].id);

    std::shared_ptr<Publication> sessionPub = members[st.connectedViaMember].pub;

    // keepalive frame (SessionKeepAlive, templateId 5)
    char kaBuf[64];
    cc::SessionKeepAlive ka;
    ka.wrapAndApplyHeader(kaBuf, 0, sizeof(kaBuf))
        .leadershipTermId(st.leadershipTermId)
        .clusterSessionId(st.clusterSessionId);
    const util::index_t kaLen = (util::index_t)(cc::MessageHeader::encodedLength() + ka.encodedLength());
    AtomicBuffer kaAb(reinterpret_cast<std::uint8_t *>(kaBuf), (size_t)kaLen);
    sessionPub->offer(kaAb, 0, kaLen);

    // ---------------- goal 2: offer one NewOrder (match CreateOrder, schema v9) ----------------
    char orderBuf[256];
    cc::SessionMessageHeader smh;
    smh.wrapAndApplyHeader(orderBuf, 0, sizeof(orderBuf))
        .leadershipTermId(st.leadershipTermId)
        .clusterSessionId(st.clusterSessionId)
        .timestamp(0); // Java client leaves headerBuffer timestamp at 0
    const std::uint64_t clusterFrameLen = cc::MessageHeader::encodedLength() + smh.encodedLength();

    mm::CreateOrder order;
    order.wrapAndApplyHeader(orderBuf, clusterFrameLen, sizeof(orderBuf) - clusterFrameLen)
        .userId(ORDER_USER_ID)
        .price(ORDER_PRICE)
        .quantity(ORDER_QTY)
        .marketId(ORDER_MARKET_ID)
        .orderType(mm::OrderType::LIMIT)
        .orderSide(mm::OrderSide::BID)
        .omsOrderId(ORDER_OMS_ORDER_ID);
    const util::index_t orderLen = (util::index_t)(
        clusterFrameLen + mm::MessageHeader::encodedLength() + order.encodedLength());

    AtomicBuffer orderAb(reinterpret_cast<std::uint8_t *>(orderBuf), (size_t)orderLen);
    std::int64_t offerResult = -1;
    const std::int64_t offerDeadline = nowMs() + 5000;
    while (nowMs() < offerDeadline)
    {
        offerResult = sessionPub->offer(orderAb, 0, orderLen);
        if (offerResult > 0)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (offerResult <= 0)
    {
        std::printf("FAIL: CreateOrder offer result=%lld\n", (long long)offerResult);
        return 1;
    }
    std::printf("OFFERED CreateOrder len=%d position=%lld marketId=%d side=BID type=LIMIT "
        "price=%lld qty=%lld userId=%lld omsOrderId=%lld schema=1 v9\n",
        (int)orderLen, (long long)offerResult, ORDER_MARKET_ID,
        (long long)ORDER_PRICE, (long long)ORDER_QTY,
        (long long)ORDER_USER_ID, (long long)ORDER_OMS_ORDER_ID);

    // ---------------- goal 3: receive egress ----------------
    const std::int64_t egressDeadline = nowMs() + 20000;
    std::int64_t lastKeepAlive = nowMs();
    while (nowMs() < egressDeadline && !st.orderResponseSeen && !st.fatal)
    {
        egressSub->poll(assembling, 10);
        if (nowMs() - lastKeepAlive > 500)
        {
            sessionPub->offer(kaAb, 0, kaLen);
            lastKeepAlive = nowMs();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::printf("SUMMARY egressMessages=%ld orderResponseSeen=%d anyAppEgress=%d\n",
        st.egressCount, st.orderResponseSeen ? 1 : 0, st.anyAppEgress ? 1 : 0);

    if (st.orderResponseSeen)
    {
        std::printf("PASS: connect + ingress CreateOrder + egress OrderStatus round-trip on schema v9\n");
        return 0;
    }
    if (st.anyAppEgress)
    {
        std::printf("PASS(weak): connected and decoded app-level egress, but no OrderStatus for "
            "omsOrderId=%lld — check node logs for ingress drop counters\n",
            (long long)ORDER_OMS_ORDER_ID);
        return 0;
    }
    std::printf("FAIL: no app-level egress decoded\n");
    return 1;
}
