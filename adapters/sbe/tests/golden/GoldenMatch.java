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
                .orderSide((short) 1);
        print("JAVA_DURABLE_ORDER_COMMAND", buf, header.encodedLength() + command.encodedLength());
    }

    public static void main(final String[] args) {
        match();
    }
}
