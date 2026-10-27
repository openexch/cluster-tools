// SPDX-License-Identifier: Apache-2.0
// Prints golden wire frames encoded by the ME (match) engine's OWN generated Java encoders as C++
// initialisers for adapters/sbe/tests/CodecLayoutTest.cpp (run by regen.sh). Field values are
// fixed and distinct per field so a swapped or shifted field cannot pass.

import org.agrona.concurrent.UnsafeBuffer;

public final class GoldenMatch {

    private static void print(final String name, final UnsafeBuffer buf, final int length) {
        final StringBuilder sb = new StringBuilder();
        sb.append("static const std::vector<std::uint8_t> ").append(name).append(" = {");
        for (int i = 0; i < length; i++) {
            if (i % 16 == 0) {
                sb.append("\n    ");
            }
            sb.append(String.format("0x%02x,", buf.getByte(i) & 0xFF));
            if (i % 16 != 15 && i != length - 1) {
                sb.append(' ');
            }
        }
        sb.append("\n};");
        System.out.println(sb);
    }

    private static void match() {
        final UnsafeBuffer buf = new UnsafeBuffer(new byte[1024]);
        final com.match.infrastructure.generated.MessageHeaderEncoder header =
                new com.match.infrastructure.generated.MessageHeaderEncoder();
        final var command = new com.match.infrastructure.generated.DurableOrderCommandEncoder()
                .wrapAndApplyHeader(buf, 0, header)
                .commandIdHigh(2001L)
                .commandIdLow(5L)
                .userId(3001L)
                .omsOrderId(4001L)
                .oldOrderId(5001L)
                .price(700_000_001L)
                .quantity(800_000_001L)
                .budget(900_000_001L)
                .marketId(7)
                .commandKind((short) 1)
                .orderType((short) 1)
                .orderSide((short) 1)
                // v12 tail: native time-in-force and the expected remaining of the amended order
                .timeInForce((short) 2)
                .expectedOldRemaining(1_100_000_001L);
        print("JAVA_DURABLE_ORDER_COMMAND", buf, header.encodedLength() + command.encodedLength());

        // ClusterHeartbeat v12: root block 9 -> 17 (meEpoch appended). Consumers decode with the
        // frame's own blockLength + version (RC2 S1 review-match row 10).
        final var heartbeat = new com.match.infrastructure.generated.ClusterHeartbeatEncoder()
                .wrapAndApplyHeader(buf, 0, header)
                .nodeId((short) 2)
                .timestamp(1_700_000_000_123L)
                .meEpoch(0x5152535455565758L);
        print("JAVA_CLUSTER_HEARTBEAT", buf, header.encodedLength() + heartbeat.encodedLength());

        final var statuses = new com.match.infrastructure.generated.OrderStatusBatchEncoder()
                .wrapAndApplyHeader(buf, 0, header)
                .marketId(7)
                .timestamp(1_700_000_000_456L);
        final var orders = statuses.ordersCount(2);
        for (int i = 0; i < 2; i++) {
            orders.next()
                    .orderId(9000L + i)
                    .userId(3000L + i)
                    .status(com.match.infrastructure.generated.OrderStatus.PARTIALLY_FILLED)
                    .price(700_000_000L + i)
                    .remainingQty(100L + i)
                    .filledQty(200L + i)
                    .side(com.match.infrastructure.generated.OrderSide.ASK)
                    .omsOrderId(4000L + i)
                    .statusSeq(500L + i)
                    .rejectReason((short) 0)
                    .egressSeq(600L + i)
                    .clusterTimestamp(1_700_000_000_000L + i);
        }
        print("JAVA_ORDER_STATUS_BATCH", buf, header.encodedLength() + statuses.encodedLength());

        // EngineConfig v12 (root block 17 -> 35): what cluster-tools' EngineConfigCodec must reproduce
        // byte for byte (EngineConfigCodecTest). Markets in ascending marketId, as normalized().
        final var config = new com.match.infrastructure.generated.EngineConfigEncoder()
                .wrapAndApplyHeader(buf, 0, header)
                .configVersion(7L)
                .impl(com.match.infrastructure.generated.EngineImpl.ARRAY)
                .bookCapacity(131_072L)
                .maxMatchesPerOrder(10_000L)
                .maxOrdersPerLevel(0L)
                .behaviorVersion(3L)
                .commandLedgerCapacity(100_000L)
                .genesisId(0x0102030405060708L)
                .maxWireVersion(12);
        final var markets = config.marketsCount(2);
        markets.next().marketId(1).minPrice(5_000_000_000_000L).maxPrice(15_000_000_000_000L).tickSize(100_000_000L)
                .symbol("BTC-USD");
        markets.next().marketId(2).minPrice(100_000_000_000L).maxPrice(1_000_000_000_000L).tickSize(50_000_000L)
                .symbol("ETH-USD");
        print("JAVA_ENGINE_CONFIG_V12", buf, header.encodedLength() + config.encodedLength());
    }

    // ---- RC2 S12 (contract 11.2): what the forwarder must put on the ME wire ----

    /** The 14 DurableOrderCommand fields: high, low, user, oms, old, price, qty, budget, market, kind, type, side, tif, eor. */
    private static int command(final UnsafeBuffer buf, final long[] c) {
        final com.match.infrastructure.generated.MessageHeaderEncoder header =
                new com.match.infrastructure.generated.MessageHeaderEncoder();
        final var e = new com.match.infrastructure.generated.DurableOrderCommandEncoder()
                .wrapAndApplyHeader(buf, 0, header)
                .commandIdHigh(c[0]).commandIdLow(c[1]).userId(c[2]).omsOrderId(c[3]).oldOrderId(c[4])
                .price(c[5]).quantity(c[6]).budget(c[7]).marketId((int) c[8])
                .commandKind((short) c[9]).orderType((short) c[10]).orderSide((short) c[11])
                .timeInForce((short) c[12]).expectedOldRemaining(c[13]);
        return header.encodedLength() + e.encodedLength();
    }

    private static int release(final UnsafeBuffer buf, final long releaseSeq, final long[][] entries) {
        final com.match.infrastructure.generated.MessageHeaderEncoder header =
                new com.match.infrastructure.generated.MessageHeaderEncoder();
        final var e = new com.match.infrastructure.generated.ReleaseCommandIdsEncoder()
                .wrapAndApplyHeader(buf, 0, header).releaseSeq(releaseSeq);
        final var g = e.releasesCount(entries.length);
        for (final long[] r : entries) {
            g.next().commandIdHigh(r[0]).idLowFloor(r[1])
                    .scope(com.match.infrastructure.generated.ReleaseScope.get((short) r[2]));
        }
        return header.encodedLength() + e.encodedLength();
    }

    /**
     * The S12 INVALID carrier (contract 5.5), written here independently of the C++ code: a pure function of
     * the dispatch's 14 command fields (kind raw). Returned in the command() field order.
     */
    private static long[] carrier(final long high, final long low, final long user, final long oms, final long old,
                                  final long price, final long qty, final long budget, final long market, final long kind,
                                  final long side, final long eor) {
        final long k = kind >= 0 && kind <= 2 ? kind : ((low & 3) <= 2 ? (low & 3) : 0);
        return new long[] {
            high, low,
            user >= 1 ? user : 1,
            oms >= 1 ? oms : (high >= 1 ? high : 1),
            k == 0 ? 0 : (old >= 1 ? old : 1),
            price, qty, budget,
            market >= 1 ? market : 1,
            k, 2, side >= 0 && side <= 1 ? side : 0, 1,
            k == 2 ? (eor >= 0 ? eor : 0) : 0,
        };
    }

    private static void forwarder() {
        final UnsafeBuffer buf = new UnsafeBuffer(new byte[2048]);
        // The 5.2 one-to-one mapping of GoldenMoney.E2E entries 1-3 (dispatchSeq and releaseScope dropped).
        final long[] create = { 1001, (3 << 2) | 0, 3001, 1001, 0, 700_000_001L, 800_000_001L, 900_000_001L, 7, 0, 0, 1, 1, 0 };
        final long[] cancel = { 1002, (5 << 2) | 1, 3002, 1002, 5002, 700_000_002L, 800_000_002L, 0, 8, 1, 2, 0, 0, 0 };
        final long[] amend = { 1003, (7 << 2) | 2, 3003, 1003, 5003, 700_000_003L, 800_000_003L, 0, 9, 2, 0, 1, 0, 1_100_000_003L };
        print("JAVA_DURABLE_ORDER_COMMAND_V12_CREATE", buf, command(buf, create));
        print("JAVA_DURABLE_ORDER_COMMAND_V12_CANCEL", buf, command(buf, cancel));
        print("JAVA_DURABLE_ORDER_COMMAND_V12_AMEND", buf, command(buf, amend));

        // The carrier of the E2E CREATE with userId 0 (class C, rule R2), proven INVALID by match's own rules.
        final long[] c = carrier(1001, (3 << 2) | 0, 0, 1001, 0, 700_000_001L, 800_000_001L, 900_000_001L, 7, 0, 1, 0);
        if (!com.match.domain.commands.DurableOrderIntent.isValid(c[0], c[1], c[2], c[3], c[4], (int) c[8], (int) c[9],
                (int) c[10], (int) c[11], (int) c[12], c[13])) {
            throw new IllegalStateException("carrier is not reader-valid");
        }
        final int cause = com.match.domain.commands.DurableOrderIntent.behaviourTwoCause(c[0], c[1], c[3], (int) c[9],
                (int) c[10], (int) c[12], c[13]);
        if (cause != com.match.domain.commands.DurableOrderIntent.B2_INVALID_TIF) {
            throw new IllegalStateException("carrier behaviourTwoCause " + cause + ", want 4");
        }
        print("JAVA_DURABLE_ORDER_COMMAND_V12_CARRIER", buf, command(buf, c));

        print("JAVA_RELEASE_COMMAND_IDS_V12_1", buf, release(buf, 504, new long[][] { { 1004, (9 << 2) | 1, 0 } }));
        print("JAVA_RELEASE_COMMAND_IDS_V12_E2E", buf,
                release(buf, 504, new long[][] { { 1004, (9 << 2) | 1, 0 }, { 1005, (11 << 2) | 3, 1 } }));
        final long[][] many = new long[48][];
        for (int i = 0; i < 48; i++) {
            many[i] = new long[] { 5000 + i, ((long) i << 2) | (i % 3), i % 2 };
        }
        print("JAVA_RELEASE_COMMAND_IDS_V12_48", buf, release(buf, 9000, many));

        final com.match.infrastructure.generated.MessageHeaderEncoder header =
                new com.match.infrastructure.generated.MessageHeaderEncoder();
        final var heartbeat = new com.match.infrastructure.generated.ClusterHeartbeatEncoder()
                .wrapAndApplyHeader(buf, 0, header)
                .nodeId((short) 1)
                .timestamp(1_700_000_000_999L)
                .meEpoch(0x0A1B2C3D4E5F6071L);
        print("JAVA_CLUSTER_HEARTBEAT_V12", buf, header.encodedLength() + heartbeat.encodedLength());
    }

    public static void main(final String[] args) {
        match();
        forwarder();
    }
}
