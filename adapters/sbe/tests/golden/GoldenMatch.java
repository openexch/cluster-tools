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
        // frame's own blockLength + version.
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

    // ---- what the forwarder must put on the ME wire ----

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
     * The INVALID carrier, written here independently of the C++ code: a pure function of
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

    // ---- the book egress the stitcher decodes ----
    private static final com.match.infrastructure.generated.MessageHeaderEncoder BOOK_HEADER =
            new com.match.infrastructure.generated.MessageHeaderEncoder();

    private static com.match.infrastructure.generated.BookSnapshotEncoder snapshot(final UnsafeBuffer buf, final int market,
                                                                                    final long version) {
        return new com.match.infrastructure.generated.BookSnapshotEncoder().wrapAndApplyHeader(buf, 0, BOOK_HEADER)
                .marketId(market).timestamp(1_700_000_000_000L + version).bidVersion(version - 2).askVersion(version - 1)
                .bookVersion(version);
    }

    private static void book() {
        final UnsafeBuffer buf = new UnsafeBuffer(new byte[4096]);

        // 20 bids descending from 14.99 and 20 asks ascending from 15.00, distinct qty and count per level
        final var full = snapshot(buf, 3, 0x0000_0102_0304_0506L);
        final var bids = full.bidsCount(20);
        for (int i = 0; i < 20; i++) {
            bids.next().price(1_499_000_000L - i * 1_000_000L).quantity(100_000_000L + i).orderCount(1 + i);
        }
        final var asks = full.asksCount(20);
        for (int i = 0; i < 20; i++) {
            asks.next().price(1_500_000_000L + i * 1_000_000L).quantity(200_000_000L + i).orderCount(21 + i);
        }
        print("JAVA_BOOK_SNAPSHOT_20x20", buf, BOOK_HEADER.encodedLength() + full.encodedLength());

        // A versioned book that emptied (both groups empty, bookVersion > 0)
        final var empty = snapshot(buf, 3, 777L);
        empty.bidsCount(0);
        empty.asksCount(0);
        print("JAVA_BOOK_SNAPSHOT_EMPTY", buf, BOOK_HEADER.encodedLength() + empty.encodedLength());

        // rule U (4.2): bookVersion 0, both groups empty (a restored or never-touched market on an unfixed ME)
        final var unversioned = snapshot(buf, 4, 0L);
        unversioned.bidsCount(0);
        unversioned.asksCount(0);
        print("JAVA_BOOK_SNAPSHOT_UNVERSIONED", buf, BOOK_HEADER.encodedLength() + unversioned.encodedLength());

        // 80 changes: per side 20 NEW (in front of the DELETEs, MP:911-948) then 20 DELETE
        final var d80 = new com.match.infrastructure.generated.BookDeltaEncoder().wrapAndApplyHeader(buf, 0, BOOK_HEADER)
                .marketId(3).timestamp(1_700_000_000_555L).bidVersion(40).askVersion(41)
                .bookVersion(0x0000_0200_0000_0050L).fromVersion(0x0000_0200_0000_0001L);
        final var ch = d80.changesCount(80);
        for (final com.match.infrastructure.generated.OrderSide side : new com.match.infrastructure.generated.OrderSide[] {
                com.match.infrastructure.generated.OrderSide.BID, com.match.infrastructure.generated.OrderSide.ASK }) {
            final long base = side == com.match.infrastructure.generated.OrderSide.BID ? 1_400_000_000L : 1_600_000_000L;
            for (int i = 0; i < 20; i++) {
                ch.next().price(base + i * 1_000_000L).quantity(300_000_000L + i).orderCount(1 + i).side(side)
                        .updateType(com.match.infrastructure.generated.BookUpdateType.NEW_LEVEL);
            }
            for (int i = 0; i < 20; i++) {
                ch.next().price(base - (i + 1) * 1_000_000L).quantity(0L).orderCount(0).side(side)
                        .updateType(com.match.infrastructure.generated.BookUpdateType.DELETE_LEVEL);
            }
        }
        print("JAVA_BOOK_DELTA_80", buf, BOOK_HEADER.encodedLength() + d80.encodedLength());

        // raw enums: side 2 and updateType 3 do not exist; written raw at the encoding offsets (entry 1 of 2)
        final var raw = new com.match.infrastructure.generated.BookDeltaEncoder().wrapAndApplyHeader(buf, 0, BOOK_HEADER)
                .marketId(3).timestamp(1_700_000_000_666L).bidVersion(1).askVersion(2)
                .bookVersion(2_000L).fromVersion(1_000L);
        final var rawCh = raw.changesCount(2);
        rawCh.next().price(1_499_000_000L).quantity(1L).orderCount(1)
                .side(com.match.infrastructure.generated.OrderSide.BID)
                .updateType(com.match.infrastructure.generated.BookUpdateType.NEW_LEVEL);
        rawCh.next().price(1_500_000_000L).quantity(2L).orderCount(2)
                .side(com.match.infrastructure.generated.OrderSide.ASK)
                .updateType(com.match.infrastructure.generated.BookUpdateType.UPDATE_LEVEL);
        final int groupStart = BOOK_HEADER.encodedLength() + com.match.infrastructure.generated.BookDeltaEncoder.BLOCK_LENGTH + 4;
        final int entryBlock = com.match.infrastructure.generated.BookDeltaEncoder.ChangesEncoder.sbeBlockLength();
        buf.putByte(groupStart + entryBlock + com.match.infrastructure.generated.BookDeltaEncoder.ChangesEncoder.sideEncodingOffset(),
                (byte) 2);
        buf.putByte(groupStart + entryBlock + com.match.infrastructure.generated.BookDeltaEncoder.ChangesEncoder.updateTypeEncodingOffset(),
                (byte) 3);
        print("JAVA_BOOK_DELTA_RAW_ENUMS", buf, BOOK_HEADER.encodedLength() + raw.encodedLength());

        // 26: three trades, ids 41, 43, 42 (the max is not last), distinct prices
        final var tb = new com.match.infrastructure.generated.TradeExecutionBatchEncoder().wrapAndApplyHeader(buf, 0, BOOK_HEADER)
                .marketId(3).timestamp(1_700_000_000_777L);
        final var trades = tb.tradesCount(3);
        final long[] ids = { 41L, 43L, 42L };
        for (int i = 0; i < 3; i++) {
            trades.next().tradeId(ids[i]).takerOrderId(9_000L + i).makerOrderId(9_100L + i).takerUserId(3_000L + i)
                    .makerUserId(3_100L + i).price(1_499_000_000L + 10L * i).quantity(50_000_000L + i)
                    .takerSide(com.match.infrastructure.generated.OrderSide.ASK).takerOmsOrderId(4_000L + i)
                    .makerOmsOrderId(4_100L + i).egressSeq(600L + i).clusterTimestamp(1_700_000_000_000L + i);
        }
        print("JAVA_TRADE_EXECUTION_BATCH_3", buf, BOOK_HEADER.encodedLength() + tb.encodedLength());

        // The same batch whose group header states blockLength 88 (one byte below the 89 entry): the group
        // header sits right after the root block, its blockLength is the first uint16
        buf.putShort(BOOK_HEADER.encodedLength() + com.match.infrastructure.generated.TradeExecutionBatchEncoder.BLOCK_LENGTH,
                (short) 88, java.nio.ByteOrder.LITTLE_ENDIAN);
        print("JAVA_TRADE_EXECUTION_BATCH_BAD_GROUP_BL", buf, BOOK_HEADER.encodedLength() + tb.encodedLength());
    }

    public static void main(final String[] args) {
        match();
        forwarder();
        book();
    }
}
