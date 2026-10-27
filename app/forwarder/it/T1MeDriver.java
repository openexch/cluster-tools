// SPDX-License-Identifier: Apache-2.0
// RC2 S12 W8, the ME half of the T1 driver (contract 11.4), run against the match jar (java -cp match-cluster.jar
// T1MeDriver.java ...), split from T1CoreDriver because the two jars pin different Aeron versions (assumption 3).
//
//   genesis <aeronDir> <ingress> <genesisId> <ledgerCapacity>
//       ME ADMIN: EngineConfig FRESH, behaviour 2, the given genesisId and commandLedgerCapacity, maxWireVersion 12,
//       market 1 (the one the T1 load trades); then waits for a ClusterHeartbeat carrying meEpoch = genesisId.
//   epoch <aeronDir> <ingress>
//       prints "EPOCH <meEpoch>" from the first ClusterHeartbeat with a non-zero epoch (an ADMIN session).
//   open-orders <aeronDir> <ingress> <outFile>
//       ME ADMIN RequestOpenOrdersSnapshot 7: writes "orderId omsOrderId" per resting leg; prints "OPEN legs=N".
//
// Credentials: T1_ME_ADMIN_KEY (keyId:ADMIN:secretHex). Exit 0 = done, 1 = the step failed.

import com.match.infrastructure.generated.ClusterHeartbeatDecoder;
import com.match.infrastructure.generated.EngineConfigEncoder;
import com.match.infrastructure.generated.EngineImpl;
import com.match.infrastructure.generated.MessageHeaderDecoder;
import com.match.infrastructure.generated.MessageHeaderEncoder;
import com.match.infrastructure.generated.OpenOrdersSnapshotDecoder;
import com.match.infrastructure.generated.RequestOpenOrdersSnapshotEncoder;
import com.openexchange.cluster.auth.ClusterCredentials;
import io.aeron.cluster.client.AeronCluster;
import io.aeron.cluster.client.EgressListener;
import io.aeron.logbuffer.Header;
import org.agrona.DirectBuffer;
import org.agrona.concurrent.UnsafeBuffer;

import java.io.PrintWriter;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.TimeUnit;

public final class T1MeDriver {

    private static final long TIMEOUT_MS = 30_000;
    // Only market 1 carries the T1 load (T1CoreDriver load), so the ME creates one book (the array book allocates
    // every price level up front: 0.01 .. 100.00 in 0.01 steps = 1e4 levels).
    private static final String[] SYMBOLS = { "BTC-USD" };

    static final class Session implements EgressListener, AutoCloseable {
        final AeronCluster client;
        final List<byte[]> frames = new ArrayList<>();
        final UnsafeBuffer out = new UnsafeBuffer(new byte[4096]);
        final MessageHeaderEncoder header = new MessageHeaderEncoder();
        long lastKeepAlive = System.currentTimeMillis();

        Session(final String aeronDir, final String ingress) throws InterruptedException {
            final ClusterCredentials credentials = ClusterCredentials.fromSpec(System.getenv("T1_ME_ADMIN_KEY"));
            AeronCluster c = null;
            RuntimeException last = null;
            final long deadline = System.currentTimeMillis() + 60_000;
            while (c == null && System.currentTimeMillis() < deadline) {
                try {
                    c = AeronCluster.connect(new AeronCluster.Context()
                            .credentialsSupplier(credentials.supplier())
                            .egressListener(this)
                            .egressChannel("aeron:udp?endpoint=localhost:0")
                            .ingressChannel("aeron:udp?term-length=4m|mtu=8192")
                            .aeronDirectoryName(aeronDir)
                            .ingressEndpoints(ingress)
                            .messageTimeoutNs(TimeUnit.SECONDS.toNanos(5)));
                } catch (final RuntimeException e) {
                    last = e;
                    Thread.sleep(500);
                }
            }
            if (c == null) {
                throw new IllegalStateException("could not connect to the ME", last);
            }
            client = c;
        }

        public void onMessage(final long sessionId, final long timestamp, final DirectBuffer buffer, final int offset,
                              final int length, final Header h) {
            final byte[] b = new byte[length];
            buffer.getBytes(offset, b);
            frames.add(b);
        }

        void poll() {
            client.pollEgress();
            final long now = System.currentTimeMillis();
            if (now - lastKeepAlive >= 500) {
                client.sendKeepAlive();
                lastKeepAlive = now;
            }
        }

        void offer(final int length) {
            final long deadline = System.currentTimeMillis() + TIMEOUT_MS;
            while (System.currentTimeMillis() < deadline) {
                if (client.offer(out, 0, length) > 0) {
                    return;
                }
                poll();
                Thread.onSpinWait();
            }
            throw new IllegalStateException("offer not accepted");
        }

        long awaitEpoch(final long want) {
            final MessageHeaderDecoder h = new MessageHeaderDecoder();
            final ClusterHeartbeatDecoder hb = new ClusterHeartbeatDecoder();
            final long deadline = System.currentTimeMillis() + TIMEOUT_MS;
            int seen = 0;
            while (System.currentTimeMillis() < deadline) {
                poll();
                for (; seen < frames.size(); seen++) {
                    final UnsafeBuffer b = new UnsafeBuffer(frames.get(seen));
                    h.wrap(b, 0);
                    if (h.templateId() == ClusterHeartbeatDecoder.TEMPLATE_ID) {
                        hb.wrapAndApplyHeader(b, 0, h);
                        if (hb.meEpoch() != 0 && (want == 0 || hb.meEpoch() == want)) {
                            return hb.meEpoch();
                        }
                    }
                }
                Thread.onSpinWait();
            }
            throw new IllegalStateException("no ClusterHeartbeat with meEpoch " + (want == 0 ? "!= 0" : want));
        }

        @Override
        public void close() {
            client.close();
        }
    }

    private static void genesis(final String aeronDir, final String ingress, final long genesisId, final long capacity)
            throws Exception {
        try (Session admin = new Session(aeronDir, ingress)) {
            final EngineConfigEncoder e = new EngineConfigEncoder().wrapAndApplyHeader(admin.out, 0, admin.header)
                    .configVersion(1L).impl(EngineImpl.ARRAY).bookCapacity(65_536L).maxMatchesPerOrder(10_000L)
                    .maxOrdersPerLevel(0L).behaviorVersion(2L).commandLedgerCapacity(capacity).genesisId(genesisId)
                    .maxWireVersion(12);
            final EngineConfigEncoder.MarketsEncoder m = e.marketsCount(SYMBOLS.length);
            for (int i = 0; i < SYMBOLS.length; i++) {
                m.next().marketId(i + 1).minPrice(1_000_000L).maxPrice(10_000_000_000L).tickSize(1_000_000L).symbol(SYMBOLS[i]);
            }
            admin.offer(MessageHeaderEncoder.ENCODED_LENGTH + e.encodedLength());
            System.out.println("ME FRESH offered genesisId=" + genesisId);
            System.out.println("EPOCH " + admin.awaitEpoch(genesisId));
        }
    }

    private static void epoch(final String aeronDir, final String ingress) throws Exception {
        try (Session admin = new Session(aeronDir, ingress)) {
            System.out.println("EPOCH " + admin.awaitEpoch(0));
        }
    }

    private static void openOrders(final String aeronDir, final String ingress, final Path outFile) throws Exception {
        try (Session admin = new Session(aeronDir, ingress)) {
            final long requestId = System.nanoTime();
            final RequestOpenOrdersSnapshotEncoder e = new RequestOpenOrdersSnapshotEncoder()
                    .wrapAndApplyHeader(admin.out, 0, admin.header).requestId(requestId);
            admin.offer(MessageHeaderEncoder.ENCODED_LENGTH + e.encodedLength());
            final MessageHeaderDecoder h = new MessageHeaderDecoder();
            final OpenOrdersSnapshotDecoder d = new OpenOrdersSnapshotDecoder();
            final List<long[]> legs = new ArrayList<>();
            boolean last = false;
            int seen = 0;
            final long deadline = System.currentTimeMillis() + TIMEOUT_MS;
            while (!last && System.currentTimeMillis() < deadline) {
                admin.poll();
                for (; seen < admin.frames.size(); seen++) {
                    final UnsafeBuffer b = new UnsafeBuffer(admin.frames.get(seen));
                    h.wrap(b, 0);
                    if (h.templateId() != OpenOrdersSnapshotDecoder.TEMPLATE_ID) {
                        continue;
                    }
                    d.wrapAndApplyHeader(b, 0, h);
                    if (d.requestId() != requestId) {
                        continue;
                    }
                    for (final OpenOrdersSnapshotDecoder.OrdersDecoder o : d.orders()) {
                        legs.add(new long[] {o.orderId(), o.omsOrderId()});
                    }
                    if (d.isLast() != 0) {
                        last = true;
                    }
                }
                Thread.onSpinWait();
            }
            if (!last) {
                throw new IllegalStateException("OpenOrdersSnapshot incomplete");
            }
            try (PrintWriter w = new PrintWriter(Files.newBufferedWriter(outFile))) {
                for (final long[] l : legs) {
                    w.println(l[0] + " " + l[1]);
                }
            }
            System.out.println("OPEN legs=" + legs.size());
        }
    }

    public static void main(final String[] args) throws Exception {
        try {
            switch (args[0]) {
                case "genesis":
                    genesis(args[1], args[2], Long.decode(args[3]), Long.parseLong(args[4]));
                    break;
                case "epoch":
                    epoch(args[1], args[2]);
                    break;
                case "open-orders":
                    openOrders(args[1], args[2], Path.of(args[3]));
                    break;
                default:
                    throw new IllegalArgumentException("unknown command " + args[0]);
            }
        } catch (final Exception e) {
            System.out.println("DRIVER FAIL " + e);
            e.printStackTrace(System.out);
            System.exit(1);
        }
        System.exit(0);
    }
}
