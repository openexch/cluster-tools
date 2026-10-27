// SPDX-License-Identifier: Apache-2.0
// Test harness only (RC2 S9a contract 9.3): an ME-journal-shaped archive for the C++ archive
// integration run. A MediaDriver and an Archive on control stream 4010 (local 4011), 1 MiB segment
// files, and a stdin control protocol, one command per line, one answer line per command:
//   RECORD [channel]         recorded exclusive publication on stream 4001 (default aeron:ipc?term-length=64k)
//                            -> RECORDING <id>
//   APPEND <file>...         each file offered as ONE message -> APPENDED <count> <position>
//   HEX <bytes>              one message from hex -> APPENDED 1 <position>
//   SYNTH <from> <count> <epoch>  trade frames patched from java-trade.bin: journalSeq @8, meEpoch @16,
//                            egressSeq @24 (64 * seq), tradeId @32 (seq) -> SYNTHED <count> <position>
//   PAD <n>                  appendPadding(n): shifts later frame boundaries -> PADDED <position>
//   STOP                     closes the publication: the recording stops -> STOPPED <id> <stopPosition>
//   EMPTY                    a recording started and stopped with no byte -> EMPTY <id>
//   PURGE <position>         purgeSegments of the current recording -> PURGED <id> <newStart>
//   HOLD_REPLAY              a live replay of the current recording held by this JVM -> HELD <replaySessionId>
//   RELEASE_REPLAY           stops it -> RELEASED
//   QUIT                     -> BYE, then exits
// An answer of ERROR <message> means the command failed. Every write waits until the archive has
// recorded it, so the C++ side never races the recording.
//
// Usage: java -cp aeron-all-1.53.1.jar JournalArchiveFixture <aeron-dir> <archive-dir> <control-port>
//            <fixture-dir> [<max-concurrent-replays>] [<control-host>]
import io.aeron.Aeron;
import io.aeron.ExclusivePublication;
import io.aeron.Subscription;
import io.aeron.archive.Archive;
import io.aeron.archive.ArchiveThreadingMode;
import io.aeron.archive.client.AeronArchive;
import io.aeron.archive.status.RecordingPos;
import io.aeron.driver.MediaDriver;
import io.aeron.driver.ThreadingMode;
import org.agrona.concurrent.SleepingMillisIdleStrategy;
import org.agrona.concurrent.UnsafeBuffer;
import org.agrona.concurrent.status.CountersReader;

import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.nio.file.Path;

public class JournalArchiveFixture {
    static final int JOURNAL_STREAM = 4001;
    static final long ARCHIVE_ID_BASE = 7_000;

    static MediaDriver driver;
    static Archive archive;
    static AeronArchive client;
    static Aeron aeron;
    static ExclusivePublication publication;
    static long recordingId = -1;
    static long archiveId;
    static byte[] tradeTemplate;
    static Subscription heldSubscription;
    static long heldReplay = -1;

    public static void main(String[] args) throws Exception {
        final String aeronDir = args[0];
        final File archiveDir = new File(args[1]);
        final int port = Integer.parseInt(args[2]);
        tradeTemplate = Files.readAllBytes(Path.of(args[3], "java-trade.bin"));
        final int maxReplays = args.length > 4 ? Integer.parseInt(args[4]) : 20;
        final String host = args.length > 5 ? args[5] : "127.0.0.1";
        archiveId = ARCHIVE_ID_BASE + port;
        driver = MediaDriver.launch(new MediaDriver.Context()
                .aeronDirectoryName(aeronDir).dirDeleteOnStart(true).dirDeleteOnShutdown(true)
                .threadingMode(ThreadingMode.SHARED).sharedIdleStrategy(new SleepingMillisIdleStrategy(1)));
        archive = Archive.launch(new Archive.Context()
                .aeronDirectoryName(aeronDir).archiveDir(archiveDir).deleteArchiveOnStart(true)
                .archiveId(archiveId)
                .controlChannel("aeron:udp?endpoint=" + host + ":" + port)
                .controlStreamId(4010).localControlStreamId(4011)
                .replicationChannel("aeron:udp?endpoint=127.0.0.1:0")
                .recordingEventsEnabled(false).threadingMode(ArchiveThreadingMode.SHARED)
                .idleStrategySupplier(() -> new SleepingMillisIdleStrategy(1))
                .segmentFileLength(1024 * 1024).maxConcurrentReplays(maxReplays)
                .fileSyncLevel(0).catalogFileSyncLevel(0));
        aeron = Aeron.connect(new Aeron.Context().aeronDirectoryName(aeronDir));
        client = AeronArchive.connect(new AeronArchive.Context().aeron(aeron)
                .controlRequestChannel("aeron:ipc").controlRequestStreamId(4011)
                .controlResponseChannel("aeron:ipc"));
        System.out.println("READY " + archiveId);
        System.out.flush();
        final BufferedReader in = new BufferedReader(new InputStreamReader(System.in));
        String line;
        while ((line = in.readLine()) != null) {
            final String[] cmd = line.trim().split("\\s+");
            if (cmd.length == 0 || cmd[0].isEmpty()) {
                continue;
            }
            String answer;
            try {
                answer = handle(cmd);
            } catch (final Exception e) {
                answer = "ERROR " + e.getClass().getSimpleName() + ": " + e.getMessage();
            }
            System.out.println(answer);
            System.out.flush();
            if ("BYE".equals(answer)) {
                break;
            }
        }
        if (null != publication) {
            publication.close();
        }
        client.close();
        aeron.close();
        archive.close();
        driver.close();
    }

    static String handle(final String[] cmd) throws Exception {
        switch (cmd[0]) {
            case "RECORD":
                return "RECORDING " + record(cmd.length > 1 ? cmd[1] : "aeron:ipc?term-length=64k");
            case "APPEND": {
                for (int i = 1; i < cmd.length; i++) {
                    offer(Files.readAllBytes(Path.of(cmd[i])));
                }
                return "APPENDED " + (cmd.length - 1) + " " + awaitRecorded();
            }
            case "HEX": {
                offer(hex(cmd[1]));
                return "APPENDED 1 " + awaitRecorded();
            }
            case "SYNTH": {
                final long from = Long.parseLong(cmd[1]);
                final long count = Long.parseLong(cmd[2]);
                final long epoch = Long.parseLong(cmd[3]);
                final UnsafeBuffer frame = new UnsafeBuffer(tradeTemplate.clone());
                for (long seq = from; seq < from + count; seq++) {
                    frame.putLong(8, seq, ByteOrder.LITTLE_ENDIAN);
                    frame.putLong(16, epoch, ByteOrder.LITTLE_ENDIAN);
                    frame.putLong(24, 64 * seq, ByteOrder.LITTLE_ENDIAN);
                    frame.putLong(32, seq, ByteOrder.LITTLE_ENDIAN);
                    offer(frame, tradeTemplate.length);
                }
                return "SYNTHED " + count + " " + awaitRecorded();
            }
            case "PAD": {
                final int n = Integer.parseInt(cmd[1]);
                long result;
                while ((result = publication.appendPadding(n)) < 0) {
                    if (result == ExclusivePublication.CLOSED || result == ExclusivePublication.MAX_POSITION_EXCEEDED) {
                        throw new IllegalStateException("appendPadding: " + result);
                    }
                    Thread.onSpinWait();
                }
                return "PADDED " + awaitRecorded();
            }
            case "STOP": {
                final long id = recordingId;
                publication.close();
                publication = null;
                long stop;
                final long deadline = System.nanoTime() + 10_000_000_000L;
                while ((stop = client.getStopPosition(id)) == AeronArchive.NULL_POSITION) {
                    if (System.nanoTime() > deadline) {
                        throw new IllegalStateException("recording " + id + " did not stop");
                    }
                    Thread.sleep(1);
                }
                return "STOPPED " + id + " " + stop;
            }
            case "EMPTY": {
                record("aeron:ipc?term-length=64k");
                final String stopped = handle(new String[]{"STOP"});
                return "EMPTY " + stopped.split(" ")[1];
            }
            case "PURGE": {
                final long position = Long.parseLong(cmd[1]);
                client.purgeSegments(recordingId, position);
                return "PURGED " + recordingId + " " + client.getStartPosition(recordingId);
            }
            case "HOLD_REPLAY": {
                heldSubscription = aeron.addSubscription("aeron:ipc", 9999);
                heldReplay = client.startReplay(recordingId, 0, Long.MAX_VALUE, "aeron:ipc", 9999);
                final long deadline = System.nanoTime() + 10_000_000_000L;
                while (!heldSubscription.isConnected()) {
                    if (System.nanoTime() > deadline) {
                        throw new IllegalStateException("held replay never connected");
                    }
                    Thread.sleep(1);
                }
                return "HELD " + heldReplay;
            }
            case "RELEASE_REPLAY": {
                client.stopReplay(heldReplay);
                heldSubscription.close();
                heldReplay = -1;
                return "RELEASED";
            }
            case "QUIT":
                return "BYE";
            default:
                throw new IllegalArgumentException("unknown command " + cmd[0]);
        }
    }

    static long record(final String channel) throws Exception {
        if (null != publication) {
            throw new IllegalStateException("a recording is already open");
        }
        publication = client.addRecordedExclusivePublication(channel, JOURNAL_STREAM);
        final CountersReader counters = aeron.countersReader();
        final long deadline = System.nanoTime() + 10_000_000_000L;
        int counterId;
        while ((counterId = RecordingPos.findCounterIdBySession(counters, publication.sessionId(), archiveId))
                == CountersReader.NULL_COUNTER_ID) {
            if (System.nanoTime() > deadline) {
                throw new IllegalStateException("recording never started");
            }
            Thread.sleep(1);
        }
        recordingId = RecordingPos.getRecordingId(counters, counterId);
        return recordingId;
    }

    static void offer(final byte[] bytes) {
        offer(new UnsafeBuffer(bytes), bytes.length);
    }

    static void offer(final UnsafeBuffer buffer, final int length) {
        long result;
        while ((result = publication.offer(buffer, 0, length)) < 0) {
            if (result == ExclusivePublication.CLOSED || result == ExclusivePublication.MAX_POSITION_EXCEEDED) {
                throw new IllegalStateException("offer: " + result);
            }
            Thread.onSpinWait();
        }
    }

    static long awaitRecorded() throws Exception {
        final long target = publication.position();
        final CountersReader counters = aeron.countersReader();
        final int counterId = RecordingPos.findCounterIdByRecording(counters, recordingId, archiveId);
        final long deadline = System.nanoTime() + 60_000_000_000L;
        while (counters.getCounterValue(counterId) < target) {
            if (System.nanoTime() > deadline) {
                throw new IllegalStateException("recording did not reach " + target);
            }
            Thread.sleep(1);
        }
        return target;
    }

    static byte[] hex(final String text) {
        final byte[] out = new byte[text.length() / 2];
        for (int i = 0; i < out.length; i++) {
            out[i] = (byte)Integer.parseInt(text.substring(2 * i, 2 * i + 2), 16);
        }
        return out;
    }
}
