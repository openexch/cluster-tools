// SPDX-License-Identifier: Apache-2.0
// RC2 S12 W8, the core half of the T1 driver (contract 11.4). A single-file Java program run against the assets
// jar (java -cp assets-cluster.jar T1CoreDriver.java ...): the AE and ME jars pin different Aeron versions
// (assets 1.53.0, match 1.53.1; contract assumption 3), so the driver is split into this core driver and
// T1MeDriver.java.
//
//   genesis <aeronDir> <ingress> <genesisId> <ledgerCapacity> <users>
//       ADMIN CoreConfig 1 -> 2 -> 3 (behaviour 3, genesisId); BRIDGE RegisterSession + the ME's FRESH record as
//       MeEngineConfig 68 (journalSeq 1, meEpoch = genesisId: the only feed record the T1 run needs, see below);
//       ADMIN OPEN GLOBAL (retried until the bind is in the log); TOOL deposits (USD) for users 1..N.
//       RC2 S9c W7: with a 6th argument "via-bridge" the BRIDGE session and its 68 are skipped: the ME's own FRESH record
//       reaches the core through ct-bridge (the await-bound genesis of S9c 12.5), and OPEN is retried for up to 60 s.
//   load <aeronDir> <ingress> <seconds> <perSecond> <users> <orderTag>
//       GATEWAY RegisterSession, then non-crossing LIMIT GTC BUY CreateOrders (they all rest at the ME) at the rate;
//       prints "LOAD sent=N rejects=R".
//   pending <aeronDir> <ingress> <outFile>
//       the tap: a FORWARDER session registered with dispatch (sourceEpoch = the bound epoch), RequestPendingDispatch
//       pages of 4096 from 0 until a short page; writes "seq idHigh idLow kind" per pending dispatch; prints
//       "PENDING count=N head=H".
//   capture <aeronDir> <ingress> <outDir> <seconds> <sourceEpoch>
//       RC2 S10' 7.1 item 1 (O-F5, T1-G): the tap of `pending` (a FORWARDER session registered with dispatch, sourceEpoch =
//       the bound epoch) that sends NO request and records every BROADCAST MeDispatch / MeDispatchBatch entry for <seconds>
//       into <outDir>/dispatch.map, one line "dispatchSeq commandIdHigh commandIdLow kind receiveMs" (receiveMs = this
//       JVM's wall clock at the egress poll that delivered it) in arrival order. The broadcast is emitted at mint (S12
//       D-S12-6, dense), so the map is taken WHILE the commands are pending. Prints "CAPTURE armed sourceEpoch=E" once
//       registered, then "CAPTURE count=N firstSeq=F lastSeq=L"; exit 0 when the seqs are dense over [F, L], exit 1 at a
//       gap ("CAPTURE GAP ..."; a non-dense map is not joined). A seq seen twice is counted and kept once.
//   events <aeronDir> <ingress> <outFile> <seconds>
//       RC2 S10' 7.2 T1-G (the reject reason by name: `t1-core dump --orders` prints none): a GATEWAY session (orders
//       channel) records every BROADCAST OrderEvent for <seconds> (0 = until SIGTERM), one flushed line
//       "orderId userId eventKind status reason meReason receiveMs" (enum names, UNKNOWN_<raw> for a value the schema
//       does not name). Prints "EVENTS armed" once registered, "EVENTS count=N" at the end.
//   names
//       Review F9 (2026-11-01), no cluster: the events naming on the raw values 200 (unnamed by EventKind, OrderStatus and
//       CoreRejectReason: get() throws) and 255 (the uint8 null value: NULL_VAL), plus one named value. Prints one
//       "NAMES <enum> <raw> -> <name>" line each, then "NAMES PASS" (exit 0) or "NAMES FAIL: unnamed raw not printed as
//       UNKNOWN_<raw>" (exit 1).
//
// What this T1 run does NOT have: the RC2 S9 Java bridge (contract assumption 2). At assets b05b8e8 the bridge still
// speaks the v5 settlement journal (schemaId 3: Settle / TerminalRelease), which a behaviour-3 core refuses, so no ME
// outcome ever reaches the core: every minted CREATE stays pending, and the scenarios that need outcomes (T1-A, T1-F,
// T1-G and the O-F4 "nothing pending" row) cannot close here. The 68 above is the genesis record a bridge would feed;
// it is required because the core keeps GLOBAL closed until the ME epoch is bound (X13, ME_LANE_INACTIVE).
//
// Credentials come from the environment: T1_ADMIN_KEY, T1_BRIDGE_KEY, T1_TOOL_KEY, T1_GATEWAY_KEY, T1_TAP_KEY
// (keyId:CLASS:secretHex). Exit 0 = done, 1 = the step failed (a message says why).

import com.openexchange.assets.domain.Asset;
import com.openexchange.assets.domain.FixedPoint;
import com.openexchange.assets.domain.Market;
import com.openexchange.assets.infrastructure.generated.AdminAckDecoder;
import com.openexchange.assets.infrastructure.generated.BoolFlag;
import com.openexchange.assets.infrastructure.generated.ConfigDecision;
import com.openexchange.assets.infrastructure.generated.CoreConfigEncoder;
import com.openexchange.assets.infrastructure.generated.CoreRejectReason;
import com.openexchange.assets.infrastructure.generated.CreateOrderEncoder;
import com.openexchange.assets.infrastructure.generated.DepositEncoder;
import com.openexchange.assets.infrastructure.generated.EventKind;
import com.openexchange.assets.infrastructure.generated.GateReason;
import com.openexchange.assets.infrastructure.generated.GateState;
import com.openexchange.assets.infrastructure.generated.MeDispatchBatchDecoder;
import com.openexchange.assets.infrastructure.generated.MeDispatchDecoder;
import com.openexchange.assets.infrastructure.generated.MeEngineConfigEncoder;
import com.openexchange.assets.infrastructure.generated.MessageHeaderDecoder;
import com.openexchange.assets.infrastructure.generated.MessageHeaderEncoder;
import com.openexchange.assets.infrastructure.generated.OrderEntryGateEncoder;
import com.openexchange.assets.infrastructure.generated.OrderEventBatchDecoder;
import com.openexchange.assets.infrastructure.generated.OrderEventDecoder;
import com.openexchange.assets.infrastructure.generated.OrderStatus;
import com.openexchange.assets.infrastructure.generated.OrderRejectDecoder;
import com.openexchange.assets.infrastructure.generated.OrderType;
import com.openexchange.assets.infrastructure.generated.PendingDispatchEndDecoder;
import com.openexchange.assets.infrastructure.generated.RegisterSessionEncoder;
import com.openexchange.assets.infrastructure.generated.RequestPendingDispatchEncoder;
import com.openexchange.assets.infrastructure.generated.SessionClass;
import com.openexchange.assets.infrastructure.generated.Side;
import com.openexchange.assets.infrastructure.generated.TimeInForce;
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
import java.util.BitSet;
import java.util.List;
import java.util.concurrent.TimeUnit;
import java.util.function.Supplier;

public final class T1CoreDriver {

    private static final long ONE = FixedPoint.fromDouble(1.0);
    private static final long TIMEOUT_MS = 30_000;

    /** One cluster session with an egress capture. */
    static final class Session implements EgressListener, AutoCloseable {
        final AeronCluster client;
        final List<byte[]> frames = new ArrayList<>();
        final UnsafeBuffer out = new UnsafeBuffer(new byte[8192]);
        final MessageHeaderEncoder header = new MessageHeaderEncoder();
        long lastKeepAlive = System.currentTimeMillis();

        Session(final String aeronDir, final String ingress, final String keyEnv) throws InterruptedException {
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
                            .egressChannel("aeron:udp?endpoint=" + System.getenv().getOrDefault("T1_LOCAL_HOST", "localhost") + ":0")
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
                throw new IllegalStateException("could not connect with " + keyEnv, last);
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
            throw new IllegalStateException("offer not accepted within " + TIMEOUT_MS + " ms");
        }

        /**
         * Waits for an AdminAck naming refTemplateId and correlationId; returns its result code and its reason (RC2 S17 W12
         * C6: a refused OPEN names why, e.g. 40 FEED_HALTED, 41 ME_LANE_INACTIVE, 45 HALT_HOLDER_PENDING).
         */
        Ack awaitAck(final int refTemplateId, final long correlationId) {
            final MessageHeaderDecoder h = new MessageHeaderDecoder();
            final AdminAckDecoder ack = new AdminAckDecoder();
            final long deadline = System.currentTimeMillis() + TIMEOUT_MS;
            int seen = 0;
            while (System.currentTimeMillis() < deadline) {
                for (; seen < frames.size(); seen++) {
                    final UnsafeBuffer b = new UnsafeBuffer(frames.get(seen));
                    h.wrap(b, 0);
                    if (h.templateId() == AdminAckDecoder.TEMPLATE_ID) {
                        ack.wrapAndApplyHeader(b, 0, h);
                        if (ack.refTemplateId() == refTemplateId && ack.correlationId() == correlationId) {
                            return new Ack(ack.resultRaw(), ack.reason());
                        }
                    }
                }
                poll();
                Thread.onSpinWait();
            }
            throw new IllegalStateException("no AdminAck for template " + refTemplateId + " corr " + correlationId);
        }

        void register(final long correlationId, final SessionClass cls, final boolean dispatch, final long sourceEpoch) {
            final RegisterSessionEncoder e = new RegisterSessionEncoder().wrapAndApplyHeader(out, 0, header)
                    .correlationId(correlationId).sessionClass(cls).readerKind((short) 0).resumable(BoolFlag.TRUE)
                    .identity(0x71000 + correlationId).sourceEpoch(sourceEpoch);
            e.channels().clear().acks(true).orders(!dispatch).dispatch(dispatch).control(true);
            offer(MessageHeaderEncoder.ENCODED_LENGTH + e.encodedLength());
            final int result = awaitAck(80, correlationId).result();
            if (result != 0 && result != 1) {
                throw new IllegalStateException("RegisterSession " + cls + " refused (result " + result + ")");
            }
        }

        @Override
        public void close() {
            client.close();
        }
    }

    private static void genesis(final String aeronDir, final String ingress, final long genesisId, final int ledgerCapacity,
                                final int users, final boolean viaBridge) throws Exception {
        try (Session admin = new Session(aeronDir, ingress, "T1_ADMIN_KEY")) {
            for (long v = 1; v <= 3; v++) {
                final long corr = 0x7000 + v;
                final CoreConfigEncoder e = new CoreConfigEncoder().wrapAndApplyHeader(admin.out, 0, admin.header)
                        .correlationId(corr).configVersion(v).behaviorVersion(v).genesisId(genesisId);
                final CoreConfigEncoder.MarketsEncoder m = e.marketsCount(Market.ALL.length);
                for (final Market d : Market.ALL) {
                    m.next().marketId(d.marketId()).symbol(d.symbol()).baseAssetId(d.baseAsset().id())
                            .quoteAssetId(d.quoteAsset().id()).tickSize(1).minPrice(1).maxPrice(1_000_000_000_000_000L).lotSize(0);
                }
                final CoreConfigEncoder.AssetsEncoder a = e.assetsCount(Asset.values().length);
                for (final Asset d : Asset.values()) {
                    a.next().assetId(d.id()).code(d.name()).scale((short) 8);
                }
                admin.offer(MessageHeaderEncoder.ENCODED_LENGTH + e.encodedLength());
                final int result = admin.awaitAck(70, corr).result();
                System.out.println("CoreConfig " + v + " result " + result);
                if (result != 0 && result != 1 && result != 3) { // ACCEPTED, DUPLICATE, ADOPTED
                    throw new IllegalStateException("CoreConfig " + v + " refused");
                }
            }
            if (viaBridge) {
                boolean opened = false;
                Ack last = null;
                for (long corr = 0xAE00; !opened && corr < 0xAE00 + 600; corr++) {
                    final OrderEntryGateEncoder g = new OrderEntryGateEncoder().wrapAndApplyHeader(admin.out, 0, admin.header)
                            .correlationId(corr).marketId(0).state(GateState.OPEN).reason(GateReason.ADMIN);
                    admin.offer(MessageHeaderEncoder.ENCODED_LENGTH + g.encodedLength());
                    last = admin.awaitAck(73, corr);
                    opened = last.result() == 0 || last.result() == 1;
                    if (!opened) {
                        Thread.sleep(100);
                    }
                }
                if (!opened) {
                    // RC2 S17 W12 C6 (review V6): the last refusal's reason by name; the early tries are 41 ME_LANE_INACTIVE
                    // (unbound), a latched core answers 40 FEED_HALTED (the X-7 refusal)
                    throw new IllegalStateException("ADMIN OPEN GLOBAL never accepted: last reason " + last.reason() + " "
                            + adminReasonName(last.reason()));
                }
                System.out.println("GLOBAL OPEN (bound through the bridge)");
            } else try (Session bridge = new Session(aeronDir, ingress, "T1_BRIDGE_KEY")) {
                bridge.register(0x8001, SessionClass.BRIDGE, false, 0);
                final MeEngineConfigEncoder e = new MeEngineConfigEncoder().wrapAndApplyHeader(bridge.out, 0, bridge.header)
                        .egressSeq(1).journalSeq(1).meEpoch(genesisId).configVersion(1).behaviorVersion(2)
                        .ledgerCapacity(ledgerCapacity).maxWireVersion(12).decision(ConfigDecision.FRESH);
                bridge.offer(MessageHeaderEncoder.ENCODED_LENGTH + e.encodedLength());
                boolean opened = false;
                Ack last = null;
                for (long corr = 0xAD00; !opened && corr < 0xAD00 + 100; corr++) {
                    final OrderEntryGateEncoder g = new OrderEntryGateEncoder().wrapAndApplyHeader(admin.out, 0, admin.header)
                            .correlationId(corr).marketId(0).state(GateState.OPEN).reason(GateReason.ADMIN);
                    admin.offer(MessageHeaderEncoder.ENCODED_LENGTH + g.encodedLength());
                    last = admin.awaitAck(73, corr);
                    opened = last.result() == 0 || last.result() == 1;
                    if (!opened) {
                        Thread.sleep(100);
                        bridge.poll();
                    }
                }
                if (!opened) {
                    throw new IllegalStateException("ADMIN OPEN GLOBAL never accepted: last reason " + last.reason() + " "
                            + adminReasonName(last.reason()));
                }
                System.out.println("GLOBAL OPEN");
            }
        }
        try (Session tool = new Session(aeronDir, ingress, "T1_TOOL_KEY")) {
            for (int u = 1; u <= users; u++) {
                final DepositEncoder e = new DepositEncoder().wrapAndApplyHeader(tool.out, 0, tool.header)
                        .correlationId(0x9000 + u).userId(u).assetId(Asset.USD.id()).amount(1_000_000_000L * ONE)
                        .requestIdHigh(0xD0D0).requestIdLow(u);
                tool.offer(MessageHeaderEncoder.ENCODED_LENGTH + e.encodedLength());
            }
            final long until = System.currentTimeMillis() + 2_000;
            while (System.currentTimeMillis() < until) {
                tool.poll();
                Thread.sleep(5);
            }
            System.out.println("DEPOSITED users=" + users);
        }
    }

    private static void load(final String aeronDir, final String ingress, final int seconds, final int perSecond,
                             final int users, final long tag) throws Exception {
        try (Session gw = new Session(aeronDir, ingress, "T1_GATEWAY_KEY")) {
            gw.register(0x8100 + tag, SessionClass.GATEWAY, false, 0);
            final long total = (long) seconds * perSecond;
            final long start = System.nanoTime();
            final long intervalNs = 1_000_000_000L / perSecond;
            for (long i = 0; i < total; i++) {
                final long due = start + i * intervalNs;
                while (System.nanoTime() < due) {
                    gw.poll();
                }
                final long user = 1 + (i % users);
                final long price = (1_000 + (i % 500)) * ONE / 100; // 10.00 .. 14.99, all BUY: nothing crosses
                final CreateOrderEncoder e = new CreateOrderEncoder().wrapAndApplyHeader(gw.out, 0, gw.header)
                        .correlationId(i).userId(user).clientOrderIdHigh(tag).clientOrderIdLow(i + 1)
                        .requestIdHigh(0xC000 + tag).requestIdLow(i + 1).marketId(1).side(Side.BUY)
                        .orderType(OrderType.LIMIT).timeInForce(TimeInForce.GTC).price(price).quantity(ONE).budget(0)
                        .stopPrice(0).trailingDelta(0).displayQty(0).expiresAtMs(0);
                gw.offer(MessageHeaderEncoder.ENCODED_LENGTH + e.encodedLength());
            }
            final long until = System.currentTimeMillis() + 1_000;
            while (System.currentTimeMillis() < until) {
                gw.poll();
                Thread.sleep(2);
            }
            final MessageHeaderDecoder h = new MessageHeaderDecoder();
            int rejects = 0;
            final OrderRejectDecoder r = new OrderRejectDecoder();
            for (final byte[] f : gw.frames) {
                final UnsafeBuffer b = new UnsafeBuffer(f);
                h.wrap(b, 0);
                if (h.templateId() == OrderRejectDecoder.TEMPLATE_ID) {
                    r.wrapAndApplyHeader(b, 0, h);
                    if (rejects < 5) {
                        System.out.println("REJECT ref=" + r.refTemplateId() + " reason=" + r.reasonRaw() + " detail=" + r.detail());
                    }
                    rejects++;
                }
            }
            System.out.println("LOAD sent=" + total + " rejects=" + rejects);
        }
    }

    private static void pending(final String aeronDir, final String ingress, final Path outFile, final long sourceEpoch)
            throws Exception {
        try (Session tap = new Session(aeronDir, ingress, "T1_TAP_KEY")) {
            tap.register(0x8200, SessionClass.FORWARDER, true, sourceEpoch);
            final MessageHeaderDecoder h = new MessageHeaderDecoder();
            final List<long[]> pending = new ArrayList<>();
            long from = 0;
            long head = 0;
            for (long corr = 0x6500; ; corr++) {
                tap.frames.clear();
                final RequestPendingDispatchEncoder e = new RequestPendingDispatchEncoder().wrapAndApplyHeader(tap.out, 0, tap.header)
                        .correlationId(corr).fromDispatchSeq(from).maxEntries(4096);
                tap.offer(MessageHeaderEncoder.ENCODED_LENGTH + e.encodedLength());
                final long deadline = System.currentTimeMillis() + TIMEOUT_MS;
                int count = -1;
                final List<long[]> page = new ArrayList<>();
                int seen = 0;
                while (count < 0 && System.currentTimeMillis() < deadline) {
                    tap.poll();
                    for (; seen < tap.frames.size(); seen++) {
                        final UnsafeBuffer b = new UnsafeBuffer(tap.frames.get(seen));
                        h.wrap(b, 0);
                        switch (h.templateId()) {
                            case MeDispatchDecoder.TEMPLATE_ID: {
                                final MeDispatchDecoder d = new MeDispatchDecoder().wrapAndApplyHeader(b, 0, h);
                                page.add(new long[] {d.dispatchSeq(), d.commandIdHigh(), d.commandIdLow(), d.kindRaw()});
                                break;
                            }
                            case MeDispatchBatchDecoder.TEMPLATE_ID:
                                for (final MeDispatchBatchDecoder.DispatchesDecoder d : new MeDispatchBatchDecoder()
                                        .wrapAndApplyHeader(b, 0, h).dispatches()) {
                                    page.add(new long[] {d.dispatchSeq(), d.commandIdHigh(), d.commandIdLow(), d.kindRaw()});
                                }
                                break;
                            case PendingDispatchEndDecoder.TEMPLATE_ID: {
                                final PendingDispatchEndDecoder end = new PendingDispatchEndDecoder().wrapAndApplyHeader(b, 0, h);
                                if (end.correlationId() == corr) {
                                    count = end.count();
                                    head = end.headDispatchSeq();
                                }
                                break;
                            }
                            default:
                                break;
                        }
                    }
                }
                if (count < 0) {
                    throw new IllegalStateException("no PendingDispatchEnd for " + corr);
                }
                // the reply is the last `count` entries before the end (live broadcasts may precede it)
                final List<long[]> reply = page.subList(page.size() - count, page.size());
                pending.addAll(reply);
                if (count < 4096) {
                    break;
                }
                from = reply.get(reply.size() - 1)[0];
            }
            try (PrintWriter w = new PrintWriter(Files.newBufferedWriter(outFile))) {
                for (final long[] p : pending) {
                    w.println(p[0] + " " + p[1] + " " + p[2] + " " + p[3]);
                }
            }
            System.out.println("PENDING count=" + pending.size() + " head=" + head);
        }
    }

    private static void capture(final String aeronDir, final String ingress, final Path outDir, final int seconds,
                                final long sourceEpoch) throws Exception {
        if (seconds < 1) {
            throw new IllegalArgumentException("capture <seconds> must be >= 1, got " + seconds);
        }
        Files.createDirectories(outDir);
        final Path map = outDir.resolve("dispatch.map");
        try (Session tap = new Session(aeronDir, ingress, "T1_TAP_KEY");
             PrintWriter w = new PrintWriter(Files.newBufferedWriter(map))) {
            tap.register(0x8300, SessionClass.FORWARDER, true, sourceEpoch);
            System.out.println("CAPTURE armed sourceEpoch=" + sourceEpoch);
            System.out.flush();
            final MessageHeaderDecoder h = new MessageHeaderDecoder();
            final MeDispatchDecoder one = new MeDispatchDecoder();
            final MeDispatchBatchDecoder batch = new MeDispatchBatchDecoder();
            final BitSet seen = new BitSet();
            long first = Long.MAX_VALUE;
            long last = Long.MIN_VALUE;
            long count = 0;
            long repeats = 0;
            // the register reply and everything before it is not a broadcast of the window
            tap.frames.clear();
            final long until = System.currentTimeMillis() + 1000L * seconds;
            while (System.currentTimeMillis() < until) {
                tap.poll();
                final long receiveMs = System.currentTimeMillis();
                for (final byte[] f : tap.frames) {
                    final UnsafeBuffer b = new UnsafeBuffer(f);
                    h.wrap(b, 0);
                    final List<long[]> entries = new ArrayList<>();
                    if (h.templateId() == MeDispatchDecoder.TEMPLATE_ID) {
                        one.wrapAndApplyHeader(b, 0, h);
                        entries.add(new long[] {one.dispatchSeq(), one.commandIdHigh(), one.commandIdLow(), one.kindRaw()});
                    } else if (h.templateId() == MeDispatchBatchDecoder.TEMPLATE_ID) {
                        for (final MeDispatchBatchDecoder.DispatchesDecoder d : batch.wrapAndApplyHeader(b, 0, h).dispatches()) {
                            entries.add(new long[] {d.dispatchSeq(), d.commandIdHigh(), d.commandIdLow(), d.kindRaw()});
                        }
                    }
                    for (final long[] e : entries) {
                        if (e[0] < 1 || e[0] > Integer.MAX_VALUE) {
                            throw new IllegalStateException("dispatchSeq " + e[0] + " outside 1.." + Integer.MAX_VALUE);
                        }
                        if (seen.get((int) e[0])) {
                            repeats++;
                            continue;
                        }
                        seen.set((int) e[0]);
                        w.println(e[0] + " " + e[1] + " " + e[2] + " " + e[3] + " " + receiveMs);
                        count++;
                        first = Math.min(first, e[0]);
                        last = Math.max(last, e[0]);
                    }
                }
                tap.frames.clear();
                Thread.sleep(1);
            }
            w.flush();
            if (count == 0) {
                System.out.println("CAPTURE count=0 firstSeq=- lastSeq=- repeats=" + repeats);
                return;
            }
            final int gap = seen.nextClearBit((int) first);
            System.out.println("CAPTURE count=" + count + " firstSeq=" + first + " lastSeq=" + last + " repeats=" + repeats);
            if (gap <= last) {
                final int resume = seen.nextSetBit(gap);
                System.out.println("CAPTURE GAP after seq " + (gap - 1) + " next " + resume + " (missing " + (resume - gap)
                        + "): the map is not dense");
                System.exit(1);
            }
        }
    }

    // Review F9 (2026-11-01): the SBE-generated get() maps only the null value to NULL_VAL and THROWS
    // IllegalArgumentException for any other value the schema does not name; both print UNKNOWN_<raw>, the recorder lives.
    /** An AdminAck's result and reason byte (RC2 S17 W12 C6). */
    record Ack(int result, int reason) {
    }

    /**
     * RC2 S17 W12 (review V7): an AdminAck reason by name. The domain vocabulary first (it carries the AdminAck-only codes,
     * 44 DUPLICATE_IDENTITY and, from the S17 assets jar on, 45 HALT_HOLDER_PENDING), then the money-schema enum,
     * else UNKNOWN_<raw>.
     */
    static String adminReasonName(final int raw) {
        for (final com.openexchange.assets.domain.CoreRejectReason r : com.openexchange.assets.domain.CoreRejectReason.values()) {
            if (r.code() == raw) {
                return r.name();
            }
        }
        return nameOf(() -> CoreRejectReason.get((short) raw), raw);
    }

    private static String nameOf(final Supplier<Enum<?>> of, final int raw) {
        final Enum<?> e;
        try {
            e = of.get();
        } catch (final IllegalArgumentException unnamed) {
            return "UNKNOWN_" + raw;
        }
        return e == null || "NULL_VAL".equals(e.name()) ? "UNKNOWN_" + raw : e.name();
    }

    private static void names() {
        final short[] raws = {200, 255};
        boolean ok = true;
        for (final short raw : raws) {
            final String[] got = {nameOf(() -> EventKind.get(raw), raw), nameOf(() -> OrderStatus.get(raw), raw),
                nameOf(() -> CoreRejectReason.get(raw), raw)};
            final String[] enums = {"EventKind", "OrderStatus", "CoreRejectReason"};
            for (int i = 0; i < got.length; i++) {
                System.out.println("NAMES " + enums[i] + " " + raw + " -> " + got[i]);
                ok &= ("UNKNOWN_" + raw).equals(got[i]);
            }
        }
        final String named = nameOf(() -> EventKind.get(EventKind.HOLD_CHANGED.value()), EventKind.HOLD_CHANGED.value());
        System.out.println("NAMES EventKind " + EventKind.HOLD_CHANGED.value() + " -> " + named);
        ok &= "HOLD_CHANGED".equals(named);
        if (!ok) {
            System.out.println("NAMES FAIL: unnamed raw not printed as UNKNOWN_<raw>");
            System.exit(1);
        }
        System.out.println("NAMES PASS");
    }

    private static void events(final String aeronDir, final String ingress, final Path outFile, final int seconds)
            throws Exception {
        try (Session gw = new Session(aeronDir, ingress, "T1_GATEWAY_KEY");
             PrintWriter w = new PrintWriter(Files.newBufferedWriter(outFile), true)) {
            gw.register(0x8400, SessionClass.GATEWAY, false, 0);
            gw.frames.clear();
            System.out.println("EVENTS armed");
            System.out.flush();
            final MessageHeaderDecoder h = new MessageHeaderDecoder();
            final OrderEventDecoder d = new OrderEventDecoder();
            final OrderEventBatchDecoder batch = new OrderEventBatchDecoder();
            long count = 0;
            final long until = seconds == 0 ? Long.MAX_VALUE : System.currentTimeMillis() + 1000L * seconds;
            while (System.currentTimeMillis() < until) {
                gw.poll();
                final long receiveMs = System.currentTimeMillis();
                for (final byte[] f : gw.frames) {
                    final UnsafeBuffer b = new UnsafeBuffer(f);
                    h.wrap(b, 0);
                    // Under load the core coalesces OrderEvents into OrderEventBatch (SessionEgressQueue); read both.
                    if (h.templateId() == OrderEventDecoder.TEMPLATE_ID) {
                        d.wrapAndApplyHeader(b, 0, h);
                        w.println(d.orderId() + " " + d.userId() + " " + nameOf(() -> EventKind.get(d.eventKindRaw()), d.eventKindRaw()) + " "
                                + nameOf(() -> OrderStatus.get(d.statusRaw()), d.statusRaw()) + " "
                                + nameOf(() -> CoreRejectReason.get(d.reasonRaw()), d.reasonRaw()) + " " + d.meReason() + " " + receiveMs);
                        count++;
                    } else if (h.templateId() == OrderEventBatchDecoder.TEMPLATE_ID) {
                        for (final OrderEventBatchDecoder.EventsDecoder e : batch.wrapAndApplyHeader(b, 0, h).events()) {
                            w.println(e.orderId() + " " + e.userId() + " " + nameOf(() -> EventKind.get(e.eventKindRaw()), e.eventKindRaw()) + " "
                                    + nameOf(() -> OrderStatus.get(e.statusRaw()), e.statusRaw()) + " "
                                    + nameOf(() -> CoreRejectReason.get(e.reasonRaw()), e.reasonRaw()) + " " + e.meReason() + " " + receiveMs);
                            count++;
                        }
                    }
                }
                gw.frames.clear();
                Thread.sleep(2);
            }
            System.out.println("EVENTS count=" + count);
        }
    }

    public static void main(final String[] args) throws Exception {
        try {
            switch (args[0]) {
                case "genesis":
                    genesis(args[1], args[2], Long.decode(args[3]), Integer.parseInt(args[4]), Integer.parseInt(args[5]),
                            args.length > 6 && "via-bridge".equals(args[6]));
                    break;
                case "load":
                    load(args[1], args[2], Integer.parseInt(args[3]), Integer.parseInt(args[4]), Integer.parseInt(args[5]),
                            Long.parseLong(args[6]));
                    break;
                case "pending":
                    pending(args[1], args[2], Path.of(args[3]), Long.decode(args[4]));
                    break;
                case "capture":
                    if (args.length != 6) {
                        throw new IllegalArgumentException("capture <aeronDir> <ingress> <outDir> <seconds> <sourceEpoch>");
                    }
                    capture(args[1], args[2], Path.of(args[3]), Integer.parseInt(args[4]), Long.decode(args[5]));
                    break;
                case "events":
                    if (args.length != 5) {
                        throw new IllegalArgumentException("events <aeronDir> <ingress> <outFile> <seconds>");
                    }
                    events(args[1], args[2], Path.of(args[3]), Integer.parseInt(args[4]));
                    break;
                case "names":
                    names();
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
