#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# RC2 S10' contract 7.1 item 5 / 7.2 O-F5 (S12 11.4 O-F5, first-apply order at N = 2): joins the core tap's dispatch map
# (T1CoreDriver capture: "dispatchSeq commandIdHigh commandIdLow kind receiveMs" per BROADCAST MeDispatch) with the ME
# journal (ct-journal-tail --hex FRAME lines) and checks that record 3 `appliedPosition` is ascending in dispatchSeq.
# Run with `python3 -I` (no site, no cwd on sys.path).
#
#   of5-check.py <dispatch.map> <journal.txt> [--load "LOAD sent=N rejects=R"] [--window <startMs> <endMs>]
#                [--shuffle [--seed S]]
#
# Record 3 (ME journal schema 4 CommandOutcome, block 153; frame offset = 8 B header + field offset):
#   commandIdHigh @32, commandIdLow @40, appliedPosition @103 (int64), result @136 (int32: 3 CAPACITY, 7 RELEASED).
# The first record 3 of an id in journal order is its first apply. Exempt (S12 7.1): every id with a CAPACITY record 3
# (re-offered after a T3, it applies after higher seqs) and, with --window, every id whose broadcast arrived inside the
# declared ME-leader-change window [startMs, endMs] (commands truncated by the election, re-sent after newer ones).
# Exempt ids are removed from the sequence entirely (an exempt id applied late must not raise the running maximum).
# An inversion is a checked id whose appliedPosition is not above the maximum of every lower dispatchSeq checked id.
#
# Lines: OF5 <counts>, FIRST INVERSION ... (when any), COUNT ..., and the verdict "OF5 PASS" or "OF5 FAIL: <named
# lines>". Named lines: "0 inversions" (inversions > 0), "every captured template-9 id has a record 3", "count >= sent
# - rejects", "the map is dense", "some ids are checked" (checked > 0: no joined id outside every exemption makes the
# check vacuous) and, with --window, "the window holds template-9 ids" (windowIds > 0: no dispatch crossed the declared
# leader change; review F4). With --window a LOADSTART line records the first template-9 receiveMs against the window
# start. --shuffle (the negative control) permutes the positions of the checked ids before the
# check (seeded, default 1): the "0 inversions" line must FAIL. Exit 0 PASS, 1 FAIL, 64 usage, 65 unreadable input.
import random
import sys

OFF_HIGH, OFF_LOW, OFF_APPLIED, OFF_RESULT = 32, 40, 103, 136
RECORD3_FRAME = 8 + 153
CAPACITY = 3
TEMPLATE9_KINDS = (0, 1, 2)


def usage(msg):
    print("usage: of5-check.py <dispatch.map> <journal.txt> [--load \"LOAD sent=N rejects=R\"] "
          "[--window <startMs> <endMs>] [--shuffle [--seed S]]: %s" % msg)
    sys.exit(64)


def le(b, off, n, signed=True):
    return int.from_bytes(b[off:off + n], "little", signed=signed)


def parse_args(argv):
    if len(argv) < 2:
        usage("two input files are required")
    opts = {"map": argv[0], "journal": argv[1], "load": None, "window": None, "shuffle": False, "seed": 1}
    i = 2
    while i < len(argv):
        a = argv[i]
        if a == "--load" and i + 1 < len(argv):
            opts["load"] = argv[i + 1]; i += 2
        elif a == "--window" and i + 2 < len(argv):
            try:
                opts["window"] = (int(argv[i + 1]), int(argv[i + 2]))
            except ValueError:
                usage("--window needs two integer epoch ms")
            if opts["window"][0] > opts["window"][1]:
                usage("--window start after end")
            i += 3
        elif a == "--shuffle":
            opts["shuffle"] = True; i += 1
        elif a == "--seed" and i + 1 < len(argv):
            opts["seed"] = int(argv[i + 1]); i += 2
        else:
            usage("unknown or incomplete argument %r" % a)
    return opts


def load_line(text):
    kv = dict(p.split("=", 1) for p in text.split() if "=" in p)
    try:
        return int(kv["sent"]), int(kv["rejects"])
    except (KeyError, ValueError):
        usage("--load must be the driver's line \"LOAD sent=N rejects=R\", got %r" % text)


def read_map(path):
    rows = []
    try:
        with open(path) as f:
            for n, line in enumerate(f, 1):
                if not line.strip():
                    continue
                p = line.split()
                if len(p) != 5:
                    print("OF5 FAIL: %s line %d is not 'dispatchSeq high low kind receiveMs': %r" % (path, n, line.strip()))
                    sys.exit(65)
                rows.append(tuple(int(x) for x in p))
    except OSError as e:
        print("OF5 FAIL: dispatch map unreadable: %s" % e)
        sys.exit(65)
    return rows


def read_journal(path):
    first = {}     # (high, low) -> (appliedPosition, journalSeq, result) of the first record 3
    capacity = set()
    records3 = 0
    bad = 0
    try:
        with open(path) as f:
            for line in f:
                if not line.startswith("FRAME "):
                    continue
                kv = dict(p.split("=", 1) for p in line.split()[1:] if "=" in p)
                if kv.get("template") != "3" or "hex" not in kv:
                    continue
                b = bytes.fromhex(kv["hex"])
                if len(b) < RECORD3_FRAME or le(b, 0, 2, False) != 153:
                    bad += 1
                    continue
                records3 += 1
                key = (le(b, OFF_HIGH, 8), le(b, OFF_LOW, 8))
                result = le(b, OFF_RESULT, 4)
                if result == CAPACITY:
                    capacity.add(key)
                if key not in first:
                    first[key] = (le(b, OFF_APPLIED, 8), int(kv.get("seq", "0")), result)
    except OSError as e:
        print("OF5 FAIL: journal unreadable: %s" % e)
        sys.exit(65)
    return first, capacity, records3, bad


def inversions(seq):
    """seq: [(dispatchSeq, key, appliedPosition)] in dispatchSeq order -> (count, first inversion text)."""
    count, first, best = 0, None, None
    for s, key, pos in seq:
        if best is not None and pos <= best[2]:
            count += 1
            if first is None:
                first = ("FIRST INVERSION dispatchSeq=%d id=%d/%d appliedPosition=%d <= %d of dispatchSeq=%d id=%d/%d"
                         % (s, key[0], key[1], pos, best[2], best[0], best[1][0], best[1][1]))
        if best is None or pos > best[2]:
            best = (s, key, pos)
    return count, first


def main(argv):
    o = parse_args(argv)
    rows = read_map(o["map"])
    first, capacity, records3, bad = read_journal(o["journal"])
    failed = []
    seqs = sorted(r[0] for r in rows)
    dense = bool(seqs) and len(set(seqs)) == len(seqs) and seqs[-1] - seqs[0] + 1 == len(seqs)
    if not dense:
        failed.append("the map is dense")
    rows.sort(key=lambda r: r[0])
    t9 = [r for r in rows if r[3] in TEMPLATE9_KINDS]
    missing = [r for r in t9 if (r[1], r[2]) not in first]
    if missing:
        failed.append("every captured template-9 id has a record 3")
    w = o["window"]
    inside = [r for r in t9 if w is not None and w[0] <= r[4] <= w[1]]
    insideKeys = {(r[1], r[2]) for r in inside}
    cap = [r for r in t9 if (r[1], r[2]) in capacity]
    joined = [(r[0], (r[1], r[2]), first[(r[1], r[2])][0]) for r in t9 if (r[1], r[2]) in first and (r[1], r[2]) not in capacity]
    checked = [j for j in joined if j[1] not in insideKeys]
    unexempted, _ = inversions(joined)  # the window ignored (the me-leader RED: the window is needed)
    if o["shuffle"]:
        positions = [j[2] for j in checked]
        random.Random(o["seed"]).shuffle(positions)
        checked = [(j[0], j[1], p) for j, p in zip(checked, positions)]
        print("SHUFFLE seed=%d: the appliedPositions of %d checked ids permuted (negative control)" % (o["seed"], len(checked)))
    inv, firstInv = inversions(checked)
    if inv:
        failed.append("0 inversions")
    if not checked:
        failed.append("some ids are checked")
    if w is not None and not inside:
        failed.append("the window holds template-9 ids")
    print("OF5 captured=%d firstSeq=%s lastSeq=%s template9=%d releases=%d otherKinds=%d record3=%d malformedRecord3=%d "
          "missingRecord3=%d capacityExempt=%d windowIds=%d checked=%d inversions=%d unexemptedInversions=%d" % (
              len(rows), seqs[0] if seqs else "-", seqs[-1] if seqs else "-", len(t9), sum(1 for r in rows if r[3] == 3),
              sum(1 for r in rows if r[3] not in TEMPLATE9_KINDS and r[3] != 3), records3, bad, len(missing), len(cap),
              len(inside), len(checked), inv, unexempted))
    if w is not None:
        print("WINDOW %d..%d ms: %d template-9 ids inside (exempt), %d inversions when the window is not declared"
              % (w[0], w[1], len(inside), unexempted))
        firstMs = min((r[4] for r in t9), default=None)
        print("LOADSTART first template-9 receiveMs %s, %s ms before the window start" % (
            firstMs if firstMs is not None else "-", w[0] - firstMs if firstMs is not None else "-"))
    if missing:
        print("MISSING record 3 for %d ids, e.g. %s" % (len(missing), " ".join("%d/%d@%d" % (r[1], r[2], r[0]) for r in missing[:5])))
    if firstInv:
        print(firstInv)
    if o["load"] is not None:
        sent, rejects = load_line(o["load"])
        ok = len(rows) >= sent - rejects
        print("COUNT captured %d >= sent %d - rejects %d = %d: %s" % (len(rows), sent, rejects, sent - rejects, "OK" if ok else "FAIL"))
        if not ok:
            failed.append("count >= sent - rejects")
    if failed:
        print("OF5 FAIL: " + "; ".join(failed))
        return 1
    print("OF5 PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
