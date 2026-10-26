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
                    .timeInForce((short) 2);
        }
        print("JAVA_ME_DISPATCH_BATCH", buf, header.encodedLength() + batch.encodedLength());

        final var end = new com.openexchange.assets.infrastructure.generated.PendingDispatchEndEncoder()
                .wrapAndApplyHeader(buf, 0, header)
                .correlationId(77L)
                .count(4096)
                .headDispatchSeq(123456789L);
        print("JAVA_PENDING_DISPATCH_END", buf, header.encodedLength() + end.encodedLength());
    }

    public static void main(final String[] args) {
        money();
    }
}
