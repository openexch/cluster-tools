// SPDX-License-Identifier: Apache-2.0
// Prints golden wire frames encoded by the core (assets) engine's OWN generated Java encoders as C++
// initialisers for adapters/sbe/tests/CodecLayoutTest.cpp (run by regen.sh). Field values are
// fixed and distinct per field so a swapped or shifted field cannot pass.

import org.agrona.concurrent.UnsafeBuffer;

public final class GoldenMoney {

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

    private static void money() {
        final UnsafeBuffer buf = new UnsafeBuffer(new byte[1024]);
        final com.openexchange.assets.infrastructure.generated.MessageHeaderEncoder header =
                new com.openexchange.assets.infrastructure.generated.MessageHeaderEncoder();

        final var register = new com.openexchange.assets.infrastructure.generated.RegisterSessionEncoder()
                .wrapAndApplyHeader(buf, 0, header)
                .correlationId(0x0102030405060708L)
                .sessionClass(com.openexchange.assets.infrastructure.generated.SessionClass.FORWARDER)
                .readerKind((short) 0)
                .resumable(com.openexchange.assets.infrastructure.generated.BoolFlag.TRUE)
                .identity(0x1112131415161718L)
                .sourceEpoch(0x2122232425262728L);
        register.channels().clear().dispatch(true).control(true);
        print("JAVA_REGISTER_SESSION", buf, header.encodedLength() + register.encodedLength());

        final var ack = new com.openexchange.assets.infrastructure.generated.AdminAckEncoder()
                .wrapAndApplyHeader(buf, 0, header)
                .correlationId(0x0102030405060708L)
                .refTemplateId(80)
                .result(com.openexchange.assets.infrastructure.generated.AdminResult.DUPLICATE)
                .reason((short) 7)
                .configVersion(9L);
        print("JAVA_ADMIN_ACK", buf, header.encodedLength() + ack.encodedLength());

        final var batch = new com.openexchange.assets.infrastructure.generated.MeDispatchBatchEncoder()
                .wrapAndApplyHeader(buf, 0, header);
        final var entries = batch.dispatchesCount(2);
        for (int i = 0; i < 2; i++) {
            entries.next()
                    .dispatchSeq(1000 + i)
                    .commandIdHigh(2000 + i)
                    .commandIdLow(((long) i << 2) | 1)
                    .kind(com.openexchange.assets.infrastructure.generated.DispatchKind.CANCEL)
                    .userId(3000 + i)
                    .omsOrderId(4000 + i)
                    .oldOrderId(5000 + i)
                    .marketId(6 + i)
                    .orderType((short) 1)
                    .side((short) i)
                    .price(700_000_000L + i)
                    .quantity(800_000_000L + i)
                    .budget(900_000_000L + i)
                    .timeInForce((short) 2)
                    // v7 tail (entry 80 -> 89 bytes): released-id scope and the expected remaining
                    // the ME subtracts in-window fills from (distinct, non-default values).
                    .releaseScope(com.openexchange.assets.infrastructure.generated.ReleaseScope.ORDER)
                    .expectedOldRemaining(1_100_000_000L + i);
        }
        print("JAVA_ME_DISPATCH_BATCH", buf, header.encodedLength() + batch.encodedLength());

        final var end = new com.openexchange.assets.infrastructure.generated.PendingDispatchEndEncoder()
                .wrapAndApplyHeader(buf, 0, header)
                .correlationId(77L)
                .count(4096)
                .headDispatchSeq(123456789L);
        print("JAVA_PENDING_DISPATCH_END", buf, header.encodedLength() + end.encodedLength());
    }

    // ---- RC2 S12 (contract 11.2): the forwarder's core-side frames ----

    /** One MeDispatch entry's 16 fields in schema order (dispatchSeq first). */
    private static final long[][] E2E = {
        // seq, high, low, kind, user, oms, old, market, type, side, price, qty, budget, tif, scope, eor
        { 501, 1001, (3 << 2) | 0, 0, 3001, 1001, 0,    7, 0, 1, 700_000_001L, 800_000_001L, 900_000_001L, 1, 0, 0 },
        { 502, 1002, (5 << 2) | 1, 1, 3002, 1002, 5002, 8, 2, 0, 700_000_002L, 800_000_002L, 0, 0, 0, 0 },
        { 503, 1003, (7 << 2) | 2, 2, 3003, 1003, 5003, 9, 0, 1, 700_000_003L, 800_000_003L, 0, 0, 0, 1_100_000_003L },
        { 504, 1004, (9 << 2) | 1, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 505, 1005, (11 << 2) | 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0 },
    };

    private static void fill(final com.openexchange.assets.infrastructure.generated.MeDispatchBatchEncoder.DispatchesEncoder e,
                             final long[] v) {
        e.dispatchSeq(v[0]).commandIdHigh(v[1]).commandIdLow(v[2])
                .kind(com.openexchange.assets.infrastructure.generated.DispatchKind.get((short) v[3]))
                .userId(v[4]).omsOrderId(v[5]).oldOrderId(v[6]).marketId((int) v[7])
                .orderType((short) v[8]).side((short) v[9]).price(v[10]).quantity(v[11]).budget(v[12])
                .timeInForce((short) v[13])
                .releaseScope(com.openexchange.assets.infrastructure.generated.ReleaseScope.get((short) v[14]))
                .expectedOldRemaining(v[15]);
    }

    private static void forwarder() {
        final UnsafeBuffer buf = new UnsafeBuffer(new byte[2048]);
        final com.openexchange.assets.infrastructure.generated.MessageHeaderEncoder header =
                new com.openexchange.assets.infrastructure.generated.MessageHeaderEncoder();

        final var amend = new com.openexchange.assets.infrastructure.generated.MeDispatchEncoder()
                .wrapAndApplyHeader(buf, 0, header)
                .dispatchSeq(0x0A0B0C0DL).commandIdHigh(2101L).commandIdLow((4L << 2) | 2)
                .kind(com.openexchange.assets.infrastructure.generated.DispatchKind.AMEND)
                .userId(2201L).omsOrderId(2101L).oldOrderId(2301L).marketId(24)
                .orderType((short) 2).side((short) 1).price(2501L).quantity(2601L).budget(2701L)
                .timeInForce((short) 0)
                .releaseScope(com.openexchange.assets.infrastructure.generated.ReleaseScope.UP_TO)
                .expectedOldRemaining(0x3132333435363738L);
        print("JAVA_ME_DISPATCH_AMEND_V7", buf, header.encodedLength() + amend.encodedLength());

        final var batch = new com.openexchange.assets.infrastructure.generated.MeDispatchBatchEncoder()
                .wrapAndApplyHeader(buf, 0, header);
        final var entries = batch.dispatchesCount(E2E.length);
        for (final long[] v : E2E) {
            fill(entries.next(), v);
        }
        print("JAVA_ME_DISPATCH_BATCH_V7", buf, header.encodedLength() + batch.encodedLength());

        // kind 4 and releaseScope 2 do not exist in the enums: written raw at the encoding offsets.
        final var raw = new com.openexchange.assets.infrastructure.generated.MeDispatchBatchEncoder()
                .wrapAndApplyHeader(buf, 0, header);
        final var rawEntries = raw.dispatchesCount(2);
        fill(rawEntries.next(), E2E[0]);
        fill(rawEntries.next(), E2E[3]);
        final int groupStart = header.encodedLength()
                + com.openexchange.assets.infrastructure.generated.MeDispatchBatchEncoder.BLOCK_LENGTH + 4;
        final int entryBlock = com.openexchange.assets.infrastructure.generated.MeDispatchBatchEncoder.DispatchesEncoder.sbeBlockLength();
        buf.putByte(groupStart + com.openexchange.assets.infrastructure.generated.MeDispatchBatchEncoder.DispatchesEncoder.kindEncodingOffset(),
                (byte) 4);
        buf.putByte(groupStart + entryBlock
                + com.openexchange.assets.infrastructure.generated.MeDispatchBatchEncoder.DispatchesEncoder.releaseScopeEncodingOffset(),
                (byte) 2);
        print("JAVA_ME_DISPATCH_RAW_ENUMS", buf, header.encodedLength() + raw.encodedLength());

        final var probeEnd = new com.openexchange.assets.infrastructure.generated.PendingDispatchEndEncoder()
                .wrapAndApplyHeader(buf, 0, header)
                .correlationId(0x6502L).count(0).headDispatchSeq(9999L);
        print("JAVA_PENDING_DISPATCH_END_PROBE", buf, header.encodedLength() + probeEnd.encodedLength());

        final var admission = new com.openexchange.assets.infrastructure.generated.AdmissionStateEncoder()
                .wrapAndApplyHeader(buf, 0, header)
                .marketId(0)
                .gateState(com.openexchange.assets.infrastructure.generated.GateState.SUSPENDED)
                .gateReason(com.openexchange.assets.infrastructure.generated.GateReason.BRIDGE_HALT)
                .circuitBreaker(com.openexchange.assets.infrastructure.generated.BoolFlag.FALSE)
                .atMs(1_700_000_000_777L)
                .logPosition(0x4142434445464748L);
        admission.gateReasons().clear().bridgeHalt(true).meCapacity(true);
        print("JAVA_ADMISSION_STATE_V7", buf, header.encodedLength() + admission.encodedLength());

        final com.openexchange.assets.infrastructure.generated.CoreRejectReason[] reasons = {
            com.openexchange.assets.infrastructure.generated.CoreRejectReason.INVALID,
            com.openexchange.assets.infrastructure.generated.CoreRejectReason.UNREGISTERED_SESSION,
        };
        final String[] names = { "JAVA_ORDER_REJECT_65_INVALID", "JAVA_ORDER_REJECT_65_UNREGISTERED" };
        for (int i = 0; i < 2; i++) {
            final var reject = new com.openexchange.assets.infrastructure.generated.OrderRejectEncoder()
                    .wrapAndApplyHeader(buf, 0, header)
                    .correlationId(0x6501L + i).requestIdHigh(0L).requestIdLow(0L).userId(0L).orderId(0L).marketId(0)
                    .refTemplateId(65).reason(reasons[i]).meReason((short) 255).detail(-1L)
                    .rejectedAtMs(1_700_000_000_888L);
            print(names[i], buf, header.encodedLength() + reject.encodedLength());
        }

        final var probe = new com.openexchange.assets.infrastructure.generated.RequestPendingDispatchEncoder()
                .wrapAndApplyHeader(buf, 0, header)
                .correlationId(0x6502L).fromDispatchSeq(777L).maxEntries(0);
        print("JAVA_REQUEST_PENDING_DISPATCH_PROBE", buf, header.encodedLength() + probe.encodedLength());
        final var page = new com.openexchange.assets.infrastructure.generated.RequestPendingDispatchEncoder()
                .wrapAndApplyHeader(buf, 0, header)
                .correlationId(0x6503L).fromDispatchSeq(778L).maxEntries(4096);
        print("JAVA_REQUEST_PENDING_DISPATCH_PAGE", buf, header.encodedLength() + page.encodedLength());

        // What CoreSessionRegistrar sends for the forwarder: class FORWARDER, dispatch|control, the bound ME
        // epoch as sourceEpoch, round 1 of the registrar's correlation base.
        final var register = new com.openexchange.assets.infrastructure.generated.RegisterSessionEncoder()
                .wrapAndApplyHeader(buf, 0, header)
                .correlationId(0x5E55_0000_0000L + 1)
                .sessionClass(com.openexchange.assets.infrastructure.generated.SessionClass.FORWARDER)
                .readerKind((short) 0)
                .resumable(com.openexchange.assets.infrastructure.generated.BoolFlag.TRUE)
                .identity(0x1234567890ABCDEFL)
                .sourceEpoch(0x0A1B2C3D4E5F6071L);
        register.channels().clear().dispatch(true).control(true);
        print("JAVA_REGISTER_SESSION_EPOCH", buf, header.encodedLength() + register.encodedLength());
    }

    public static void main(final String[] args) {
        money();
        forwarder();
    }
}
