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
//   load <aeronDir> <ingress> <seconds> <perSecond> <users> <orderTag>
//       GATEWAY RegisterSession, then non-crossing LIMIT GTC BUY CreateOrders (they all rest at the ME) at the rate;
//       prints "LOAD sent=N rejects=R".
//   pending <aeronDir> <ingress> <outFile>
//       the tap: a FORWARDER session registered with dispatch (sourceEpoch = the bound epoch), RequestPendingDispatch
//       pages of 4096 from 0 until a short page; writes "seq idHigh idLow kind" per pending dispatch; prints
//       "PENDING count=N head=H".
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
import com.openexchange.assets.infrastructure.generated.CreateOrderEncoder;
import com.openexchange.assets.infrastructure.generated.DepositEncoder;
import com.openexchange.assets.infrastructure.generated.GateReason;
import com.openexchange.assets.infrastructure.generated.GateState;
import com.openexchange.assets.infrastructure.generated.MeDispatchBatchDecoder;
import com.openexchange.assets.infrastructure.generated.MeDispatchDecoder;
import com.openexchange.assets.infrastructure.generated.MeEngineConfigEncoder;
import com.openexchange.assets.infrastructure.generated.MessageHeaderDecoder;
import com.openexchange.assets.infrastructure.generated.MessageHeaderEncoder;
import com.openexchange.assets.infrastructure.generated.OrderEntryGateEncoder;
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
import java.util.List;
import java.util.concurrent.TimeUnit;

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

        /** Waits for an AdminAck naming refTemplateId and correlationId; returns its result code. */
        int awaitAck(final int refTemplateId, final long correlationId) {
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
                            return ack.resultRaw();
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
            final int result = awaitAck(80, correlationId);
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
                                final int users) throws Exception {
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
                final int result = admin.awaitAck(70, corr);
                System.out.println("CoreConfig " + v + " result " + result);
                if (result != 0 && result != 1 && result != 3) { // ACCEPTED, DUPLICATE, ADOPTED
                    throw new IllegalStateException("CoreConfig " + v + " refused");
                }
            }
            try (Session bridge = new Session(aeronDir, ingress, "T1_BRIDGE_KEY")) {
                bridge.register(0x8001, SessionClass.BRIDGE, false, 0);
                final MeEngineConfigEncoder e = new MeEngineConfigEncoder().wrapAndApplyHeader(bridge.out, 0, bridge.header)
                        .egressSeq(1).journalSeq(1).meEpoch(genesisId).configVersion(1).behaviorVersion(2)
                        .ledgerCapacity(ledgerCapacity).maxWireVersion(12).decision(ConfigDecision.FRESH);
                bridge.offer(MessageHeaderEncoder.ENCODED_LENGTH + e.encodedLength());
                boolean opened = false;
                for (long corr = 0xAD00; !opened && corr < 0xAD00 + 100; corr++) {
                    final OrderEntryGateEncoder g = new OrderEntryGateEncoder().wrapAndApplyHeader(admin.out, 0, admin.header)
                            .correlationId(corr).marketId(0).state(GateState.OPEN).reason(GateReason.ADMIN);
                    admin.offer(MessageHeaderEncoder.ENCODED_LENGTH + g.encodedLength());
                    final int result = admin.awaitAck(73, corr);
                    opened = result == 0 || result == 1;
                    if (!opened) {
                        Thread.sleep(100);
                        bridge.poll();
                    }
                }
                if (!opened) {
                    throw new IllegalStateException("ADMIN OPEN GLOBAL never accepted (epoch not bound?)");
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

    public static void main(final String[] args) throws Exception {
        try {
            switch (args[0]) {
                case "genesis":
                    genesis(args[1], args[2], Long.decode(args[3]), Integer.parseInt(args[4]), Integer.parseInt(args[5]));
                    break;
                case "load":
                    load(args[1], args[2], Integer.parseInt(args[3]), Integer.parseInt(args[4]), Integer.parseInt(args[5]),
                            Long.parseLong(args[6]));
                    break;
                case "pending":
                    pending(args[1], args[2], Path.of(args[3]), Long.decode(args[4]));
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
