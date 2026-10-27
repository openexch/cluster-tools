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

    public static void main(final String[] args) {
        match();
    }
}
