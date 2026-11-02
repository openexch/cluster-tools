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
//   resend <aeronDir> <meIngress> <frame.bin> [count]
//       RC2 S10' 7.1 item 3 (T1-F re-sends, review F-6): opens an ME FORWARDER session on T1_ME_FORWARDER_KEY (the
//       runner's 8:FORWARDER, a key no forwarder uses; the ADMIN session of `epoch` is NOT reused) and takes it through
//       the forwarder's M2 -> M3 step of S12 4.2: nothing is offered until the S12 W9 epoch hello (a ClusterHeartbeat
//       with meEpoch != 0 on THIS session) arrived. Then offers the captured frame bytes (a ct_forwarder_tc_capture
//       <n>.bin: order schema 1, template 9 or 10, exactly as fwdA offered them) `count` times (default 1), polls 3 s
//       and prints the ME's reply frames on this session by template. Prints "FRAME bytes=N template=T sha256=H" first
//       (the runner compares H with the capture sidecar, N-11), "HELLO meEpoch=E after=Mms", "SENT count=C",
//       "REPLY template=T count=K" per template (or "REPLY none"). A hello refusal exits 78 with the reason
//       ("HELLO REFUSED <reason>": the credential rejected, or no hello within 10 s).
//   book <aeronDir> <ingress>
//       RC2 S13 contract 12: opens an ME ADMIN session (the open itself triggers a resnapshot, ACS:1613-1616), waits
//       for one BookSnapshot 22 of every configured market (T1_ME_MARKETS) or 2 s, prints per market
//       "BOOK market=<m> version=<v> bid=<b> ask=<a>" (bid=0 ask=0 for a versioned empty book after W-ME2) or
//       "BOOK market=<m> none" (no snapshot: the market was never versioned), closes. Raw little-endian reads.
//   listen <aeronDir> <ingress> <seconds>
//       RC2 S13 (TK-I, evidence): an ME ADMIN session for <seconds>; prints "T=<ms> tpl=22|25|26 market=<m> ..." per
//       book or trade frame (26: "maxTradeId=<id> price=<p>" of the batch's highest tradeId), raw reads.
//
// T1_ME_MARKETS (RC2 S13 W8, default 1): genesis configures markets 1..N (market 1 BTC-USD carries the load; the
// others are configured and never traded, the contract 12 "markets 2..5" lines); book waits for markets 1..N.
//
// Credentials: T1_ME_ADMIN_KEY (keyId:ADMIN:secretHex), T1_ME_FORWARDER_KEY (resend). Exit 0 = done, 1 = the step
// failed, 78 = the hello was refused (resend).

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
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.HexFormat;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.concurrent.TimeUnit;

public final class T1MeDriver {

    private static final long TIMEOUT_MS = 30_000;
    // Only market 1 carries the T1 load (T1CoreDriver load), so the ME creates one book (the array book allocates
    // every price level up front: 0.01 .. 100.00 in 0.01 steps = 1e4 levels).
    // RC2 S13 W8: T1_ME_MARKETS (1..5) configures more markets, which no command touches (the markets 2..5 lines).
    private static final String[] ALL_SYMBOLS = { "BTC-USD", "ETH-USD", "SOL-USD", "XRP-USD", "ADA-USD" };
    private static final String[] SYMBOLS = java.util.Arrays.copyOf(ALL_SYMBOLS, markets());

    private static int markets() {
        final String v = System.getenv("T1_ME_MARKETS");
        final int n = v == null || v.isEmpty() ? 1 : Integer.parseInt(v);
        if (n < 1 || n > ALL_SYMBOLS.length) {
            throw new IllegalArgumentException("T1_ME_MARKETS must be 1.." + ALL_SYMBOLS.length + ", got " + v);
        }
        return n;
    }

    static final class Session implements EgressListener, AutoCloseable {
        final AeronCluster client;
        final List<byte[]> frames = new ArrayList<>();
        final UnsafeBuffer out = new UnsafeBuffer(new byte[4096]);
        final MessageHeaderEncoder header = new MessageHeaderEncoder();
        long lastKeepAlive = System.currentTimeMillis();

        Session(final String aeronDir, final String ingress) throws InterruptedException, HelloRefused {
            this(aeronDir, ingress, "T1_ME_ADMIN_KEY", false);
        }

        /** failFastOnAuth: an authentication rejection is a HelloRefused at once (resend), not a retried connect. */
        Session(final String aeronDir, final String ingress, final String keyEnv, final boolean failFastOnAuth)
                throws InterruptedException, HelloRefused {
            final String spec = System.getenv(keyEnv);
            if (spec == null) {
                throw new IllegalStateException(keyEnv + " is not set");
            }
            final ClusterCredentials credentials = ClusterCredentials.fromSpec(spec);
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
                } catch (final io.aeron.security.AuthenticationException e) {
                    if (failFastOnAuth) {
                        throw new HelloRefused("the ME rejected the " + keyEnv + " credential: " + e.getMessage());
                    }
                    last = e;
                    Thread.sleep(500);
                } catch (final RuntimeException e) {
                    last = e;
                    Thread.sleep(500);
                }
            }
            if (c == null) {
                throw new IllegalStateException("could not connect to the ME with " + keyEnv, last);
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

    /** S10' 7.1 item 3: the hello was refused (exit 78). */
    static final class HelloRefused extends Exception {
        private static final long serialVersionUID = 1L;

        HelloRefused(final String reason) {
            super(reason);
        }
    }

    private static void resend(final String aeronDir, final String ingress, final Path frameFile, final int count)
            throws Exception {
        if (count < 1) {
            throw new IllegalArgumentException("resend [count] must be >= 1, got " + count);
        }
        final byte[] frame = Files.readAllBytes(frameFile);
        if (frame.length < MessageHeaderDecoder.ENCODED_LENGTH) {
            throw new IllegalArgumentException(frameFile + ": " + frame.length + " bytes, shorter than a message header");
        }
        final UnsafeBuffer fb = new UnsafeBuffer(frame);
        final MessageHeaderDecoder fh = new MessageHeaderDecoder().wrap(fb, 0);
        if (fh.schemaId() != MessageHeaderDecoder.SCHEMA_ID || (fh.templateId() != 9 && fh.templateId() != 10)) {
            throw new IllegalArgumentException(frameFile + ": schema " + fh.schemaId() + " template " + fh.templateId()
                    + " is not an order-schema template 9 / 10 frame");
        }
        System.out.println("FRAME bytes=" + frame.length + " template=" + fh.templateId() + " version=" + fh.version()
                + " sha256=" + HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(frame)));
        try (Session fwd = new Session(aeronDir, ingress, "T1_ME_FORWARDER_KEY", true)) {
            // M2 (S12 4.2): wait for the W9 hello on this session; nothing is offered before it
            final long start = System.currentTimeMillis();
            final MessageHeaderDecoder h = new MessageHeaderDecoder();
            final ClusterHeartbeatDecoder hb = new ClusterHeartbeatDecoder();
            long epoch = 0;
            boolean zeroSeen = false;
            int seen = 0;
            while (epoch == 0 && System.currentTimeMillis() - start < 10_000) {
                fwd.poll();
                for (; seen < fwd.frames.size() && epoch == 0; seen++) {
                    final UnsafeBuffer b = new UnsafeBuffer(fwd.frames.get(seen));
                    h.wrap(b, 0);
                    if (h.templateId() == ClusterHeartbeatDecoder.TEMPLATE_ID) {
                        final long e = hb.wrapAndApplyHeader(b, 0, h).meEpoch();
                        zeroSeen |= e == 0;
                        epoch = e;
                    }
                }
                Thread.onSpinWait();
            }
            if (epoch == 0) {
                throw new HelloRefused("no W9 epoch hello (ClusterHeartbeat with meEpoch != 0) on the FORWARDER session within 10 s"
                        + (zeroSeen ? " (only meEpoch 0: the ME is not FRESH)" : ""));
            }
            System.out.println("HELLO meEpoch=" + epoch + " after=" + (System.currentTimeMillis() - start) + "ms");
            fwd.frames.clear();
            fwd.out.putBytes(0, frame);
            for (int i = 0; i < count; i++) {
                fwd.offer(frame.length);
            }
            System.out.println("SENT count=" + count);
            final long until = System.currentTimeMillis() + 3_000;
            while (System.currentTimeMillis() < until) {
                fwd.poll();
                Thread.sleep(2);
            }
            final Map<Integer, Integer> byTemplate = new TreeMap<>();
            for (final byte[] f : fwd.frames) {
                h.wrap(new UnsafeBuffer(f), 0);
                byTemplate.merge(h.templateId(), 1, Integer::sum);
            }
            if (byTemplate.isEmpty()) {
                System.out.println("REPLY none");
            }
            byTemplate.forEach((t, n) -> System.out.println("REPLY template=" + t + " count=" + n));
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

    private static final java.nio.ByteOrder LE = java.nio.ByteOrder.LITTLE_ENDIAN;

    /** RC2 S13 contract 12: one BookSnapshot per configured market (or 2 s), then the BOOK lines. */
    private static void book(final String aeronDir, final String ingress) throws Exception {
        try (Session admin = new Session(aeronDir, ingress)) {
            final int n = SYMBOLS.length;
            final String[] line = new String[n + 1];
            int have = 0;
            int seen = 0;
            final long deadline = System.currentTimeMillis() + 2_000;
            while (have < n && System.currentTimeMillis() < deadline) {
                admin.poll();
                for (; seen < admin.frames.size(); seen++) {
                    final UnsafeBuffer b = new UnsafeBuffer(admin.frames.get(seen));
                    if (b.capacity() < 8 || (b.getShort(2, LE) & 0xFFFF) != 22) {
                        continue;
                    }
                    final int block = b.getShort(0, LE) & 0xFFFF;
                    final int p = 8;
                    final int market = b.getInt(p, LE);
                    if (market < 1 || market > n || line[market] != null) {
                        continue;
                    }
                    final long version = b.getLong(p + 28, LE);
                    final int bidsAt = p + block;
                    final int bidBlock = b.getShort(bidsAt, LE) & 0xFFFF;
                    final int nBids = b.getShort(bidsAt + 2, LE) & 0xFFFF;
                    final int asksAt = bidsAt + 4 + nBids * bidBlock;
                    final int askBlock = b.getShort(asksAt, LE) & 0xFFFF;
                    final int nAsks = b.getShort(asksAt + 2, LE) & 0xFFFF;
                    final long bid = nBids > 0 ? b.getLong(bidsAt + 4, LE) : 0;
                    final long ask = nAsks > 0 ? b.getLong(asksAt + 4, LE) : 0;
                    line[market] = "BOOK market=" + market + " version=" + version + " bid=" + bid + " ask=" + ask
                            + " bids=" + nBids + " asks=" + nAsks + " blocks=" + bidBlock + "/" + askBlock;
                    have++;
                }
                Thread.onSpinWait();
            }
            for (int m = 1; m <= n; m++) {
                System.out.println(line[m] != null ? line[m] : "BOOK market=" + m + " none");
            }
        }
    }

    /** RC2 S13 (evidence): the book and trade frames of one ADMIN session for a while. */
    private static void listen(final String aeronDir, final String ingress, final long seconds) throws Exception {
        try (Session admin = new Session(aeronDir, ingress)) {
            System.out.println("LISTEN open T=" + System.currentTimeMillis());
            int seen = 0;
            final long end = System.currentTimeMillis() + seconds * 1000;
            while (System.currentTimeMillis() < end) {
                admin.poll();
                for (; seen < admin.frames.size(); seen++) {
                    final UnsafeBuffer b = new UnsafeBuffer(admin.frames.get(seen));
                    if (b.capacity() < 8) {
                        continue;
                    }
                    final int block = b.getShort(0, LE) & 0xFFFF;
                    final int tpl = b.getShort(2, LE) & 0xFFFF;
                    final int p = 8;
                    final String t = "T=" + System.currentTimeMillis() + " tpl=" + tpl;
                    if (tpl == 22) {
                        final int nb = b.getShort(p + block + 2, LE) & 0xFFFF;
                        final int at = p + block + 4 + nb * (b.getShort(p + block, LE) & 0xFFFF);
                        System.out.println(t + " market=" + b.getInt(p, LE) + " bookVersion=" + b.getLong(p + 28, LE)
                                + " bids=" + nb + " asks=" + (b.getShort(at + 2, LE) & 0xFFFF));
                    } else if (tpl == 25) {
                        System.out.println(t + " market=" + b.getInt(p, LE) + " bookVersion=" + b.getLong(p + 28, LE)
                                + " from=" + b.getLong(p + 36, LE) + " changes=" + (b.getShort(p + block + 2, LE) & 0xFFFF));
                    } else if (tpl == 26) {
                        final int eb = b.getShort(p + block, LE) & 0xFFFF;
                        final int cnt = b.getShort(p + block + 2, LE) & 0xFFFF;
                        long maxId = 0;
                        long price = 0;
                        for (int i = 0; i < cnt; i++) {
                            final long id = b.getLong(p + block + 4 + i * eb, LE);
                            if (id > maxId) {
                                maxId = id;
                                price = b.getLong(p + block + 4 + i * eb + 40, LE);
                            }
                        }
                        System.out.println(t + " market=" + b.getInt(p, LE) + " trades=" + cnt + " maxTradeId=" + maxId
                                + " price=" + price);
                    }
                }
                Thread.onSpinWait();
            }
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
                case "book":
                    book(args[1], args[2]);
                    break;
                case "listen":
                    listen(args[1], args[2], Long.parseLong(args[3]));
                    break;
                case "resend":
                    if (args.length != 4 && args.length != 5) {
                        throw new IllegalArgumentException("resend <aeronDir> <meIngress> <frame.bin> [count]");
                    }
                    resend(args[1], args[2], Path.of(args[3]), args.length == 5 ? Integer.parseInt(args[4]) : 1);
                    break;
                default:
                    throw new IllegalArgumentException("unknown command " + args[0]);
            }
        } catch (final HelloRefused e) {
            System.out.println("HELLO REFUSED " + e.getMessage());
            System.exit(78);
        } catch (final Exception e) {
            System.out.println("DRIVER FAIL " + e);
            e.printStackTrace(System.out);
            System.exit(1);
        }
        System.exit(0);
    }
}
