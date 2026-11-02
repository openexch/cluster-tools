#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# RC2 S13 W8: the T1 tick helpers of run-forwarder-it.sh (contract section 12). Run with python3 -I.
#
#   sample <out> <interval_s> <name=url>...
#       the 100 ms sampler (tick_sample): one thread per target, each request with a 0.3 s timeout, so a stopped
#       node delays only its own thread and never the forwarder series (W0 answer 3: the dry-run sampler stretched to
#       1.1 s while an ME node was SIGSTOPped). Lines: "<t0_mono_ms> <t1_mono_ms> <name> <series> <value>", t0/t1 the
#       request start and end on CLOCK_MONOTONIC (the forwarder's log stamps are steady_clock ms, the same clock).
#       Kept series: forwarder_book_{state,version,frames_total,chain_breaks_total,recoveries_total,broken_age_ms,
#       version_regressions_total}, forwarder_tick_{offered_total,last_offered_version,pending_age_ms,suppressed_total},
#       forwarder_ticks_ready, forwarder_trade_last_id, forwarder_session_new_leader_total, assets_ticks_*,
#       assets_market_{best_bid,best_ask,book_version,last_trade_id}, match_cluster_role, match_engine_effective_impl, match_dropped_market_msgs_total,
#       match_egress_drain_capped_total, match_follower_egress_skipped_total.
#   oracle <spec.json>
#       O-T1 after the barrier (the runner's tick_oracle): prints one line per check and "O-T1 PASS" or
#       "O-T1 FAIL: <first failing lines>"; exit 0 / 1.
#   delay <fwd.log> <sampler> <fwdName> <market> <afterMonoMs> <boundMs> [--version-positive] [--which first|last]
#       the time from the forwarder's "ME new leader" log line (the first or last one after afterMonoMs) to the first
#       sample in which the market's ladder is OK (state 1) with forwarder_book_frames_total{template="22"} above its
#       value at the last sample before the line (and, with --version-positive, forwarder_book_version > 0). Prints
#       "DELAY PASS ..." / "DELAY FAIL ..." with the upper bound (the sample's end) and the lower bound (the previous
#       sample's end); exit 0 when the upper bound <= boundMs.
#   series <sampler> <name> <series> [afterMonoMs]
#       prints "<t1> <value>" of one series (evidence and the rows' own lines).
#   rise <sampler> <name> <tMonoMs> <series>...
#       per series "RISE <series> before=<v at the last sample ending before t> end=<v at the last sample>" (a series
#       family prefix ending in "{" sums every matching series).
#   logline <log> <regex> <afterMonoMs>
#       the steady_clock ms stamp of the first forwarder log line matching <regex> after afterMonoMs ("none" if absent).
#   nooffer <sampler> <name> <market>
#       RC2 S13 W9 D-9 (TK-D line 5): "no MarketTick offered for the market while its ladder is BROKEN". Over consecutive
#       samples of one target where forwarder_book_state{market} is BROKEN (2) at both ends of the window, the sum of
#       forwarder_tick_offered_total{market=...} (every edge) must not rise. Windows that start or end in another state
#       (a break or a recovery inside the window) are not judged. Prints "NOOFFER PASS|FAIL ..." (windows judged, BROKEN
#       samples, the longest judged run, violations); FAIL also when no window was judged (a vacuous pass). Exit 0 / 1.
#   changes <sampler>
#       the sampler reduced to its value changes (the evidence copy).
#   mono
#       CLOCK_MONOTONIC in ms (the runner's marks).
import json
import re
import sys
import threading
import time
import urllib.request

KEEP = re.compile(r"^(forwarder_book_(state|version|frames_total|chain_breaks_total|recoveries_total|broken_age_ms|"
                  r"version_regressions_total)\{|forwarder_tick_(offered_total|last_offered_version|pending_age_ms|"
                  r"suppressed_total)\{|forwarder_ticks_ready|forwarder_trade_last_id|forwarder_session_new_leader_total|"
                  r"assets_ticks_|assets_market_(best_bid|best_ask|book_version|last_trade_id)|match_cluster_role|"
                  r"match_engine_effective_impl|"
                  r"match_dropped_market_msgs_total|match_egress_drain_capped_total|match_follower_egress_skipped_total)")


def mono_ms():
    return int(time.monotonic() * 1000)


def sample(out, interval, targets):
    lock = threading.Lock()
    f = open(out, "a", buffering=1)

    def run(name, url):
        while True:
            t0 = mono_ms()
            try:
                body = urllib.request.urlopen(url, timeout=0.3).read().decode()
            except Exception:
                body = None
            t1 = mono_ms()
            if body is not None:
                lines = []
                for line in body.splitlines():
                    if not line or line[0] == "#" or not KEEP.match(line):
                        continue
                    p = line.rsplit(" ", 1)
                    if len(p) == 2:
                        lines.append("%d %d %s %s %s\n" % (t0, t1, name, p[0], p[1]))
                with lock:
                    f.writelines(lines)
            rest = interval - (mono_ms() - t0) / 1000.0
            if rest > 0:
                time.sleep(rest)

    for spec in targets:
        name, url = spec.split("=", 1)
        threading.Thread(target=run, args=(name, url), daemon=True).start()
    while True:
        time.sleep(3600)


def prom(path):
    d = {}
    try:
        for line in open(path):
            if not line.strip() or line[0] == "#":
                continue
            p = line.rsplit(" ", 1)
            if len(p) == 2:
                try:
                    d[p[0]] = float(p[1])
                except ValueError:
                    pass
    except OSError:
        pass
    return d


def fam(d, name, **labels):
    """sum of every series of one family whose labels include the given ones"""
    s = 0.0
    for k, v in d.items():
        if k == name and not labels:
            s += v
            continue
        if not k.startswith(name + "{"):
            continue
        ok = all(('%s="%s"' % (lk, lv)) in k for lk, lv in labels.items())
        if ok:
            s += v
    return s


def load_sampler(path):
    rows = []
    try:
        for line in open(path):
            p = line.split()
            if len(p) == 5:
                try:
                    rows.append((int(p[0]), int(p[1]), p[2], p[3], float(p[4])))
                except ValueError:
                    pass
    except OSError:
        pass
    return rows


def snapshots(rows, name):
    """the sampler rows of one target grouped per request: [(t0, t1, {series: value})]"""
    out = {}
    for t0, t1, n, s, v in rows:
        if n == name:
            out.setdefault((t0, t1), {})[s] = v
    return [(k[0], k[1], out[k]) for k in sorted(out)]


def oracle(spec_path):
    spec = json.load(open(spec_path))
    lines, fails = [], []

    def check(ok, text):
        lines.append(("PASS " if ok else "FAIL ") + text)
        if not ok:
            fails.append(text)

    if not spec["barrier_ok"]:
        check(False, "barrier: %s" % spec["barrier_detail"])
    else:
        lines.append("INFO barrier held after %.2f s: %s" % (spec["barrier_s"], spec["barrier_detail"]))
    book = {}
    for line in open(spec["book"]):
        m = re.match(r"BOOK market=(\d+) (none|version=(-?\d+) bid=(-?\d+) ask=(-?\d+))", line)
        if m:
            book[int(m.group(1))] = None if m.group(2) == "none" else (int(m.group(3)), int(m.group(4)), int(m.group(5)))
    lines.append("INFO book: " + ", ".join("%d=%s" % (k, "none" if v is None else "v%d bid %d ask %d" % v)
                                          for k, v in sorted(book.items())))
    if not book:
        check(False, "T1MeDriver book printed no BOOK line")
    leader = prom(spec["ae_leader_end"])
    mstart, mend = prom(spec["ae_member_start"]), prom(spec["ae_member_end"])
    # row_s: row start (or the forwarder's start, if later) to the oracle; fresh: started after the row start, so its
    # counters count from 0 (review-w8 FIX-2: the E10 line is judged on the row, not on the forwarder's lifetime)
    fwd = {p: ({} if f["fresh"] else prom(f["start"]), prom(f["end"]), f["row_s"]) for p, f in spec["forwarders"].items()}
    for m, v in sorted(book.items()):
        if v is None:
            continue
        ver, bid, ask = v
        cb = leader.get('assets_market_best_bid{market="%d"}' % m)
        ca = leader.get('assets_market_best_ask{market="%d"}' % m)
        cv = leader.get('assets_market_book_version{market="%d"}' % m)
        check(cb is not None and int(cb) == bid and ca is not None and int(ca) == ask,
              "market %d core quote == ME's: core bid %s ask %s, ME bid %d ask %d" % (m, cb, ca, bid, ask))
        lo = max([e.get('forwarder_tick_last_offered_version{market="%d"}' % m, 0) for _, e, _ in fwd.values()] or [0])
        check(cv is not None and int(cv) <= ver and int(cv) == int(lo),
              "market %d core version %s <= ME's %d and == max forwarder last offered %d" % (m, cv, ver, lo))
    d = lambda n: fam(mend, n) - fam(mstart, n)
    for n in ("assets_ticks_invalid_total", "assets_ticks_epoch_drop_total"):
        check(d(n) == 0, "%s delta 0 (member %s): %d" % (n, spec["ae_member"], d(n)))
    for p, (s, e, up) in sorted(fwd.items()):
        r = fam(e, "forwarder_tick_refused_total") - fam(s, "forwarder_tick_refused_total")
        check(r == 0, "fwd %s forwarder_tick_refused_total delta 0: %d" % (p, r))
    # unbound: judged only from the first rise of applied (the core is bound from then on, 7.1 / RC-F2)
    rows = snapshots(load_sampler(spec["sampler"]), spec["ae_member_name"])
    a0, u0 = fam(mstart, "assets_ticks_applied_total"), fam(mstart, "assets_ticks_unbound_total")
    first = next((snap for snap in rows if snap[2].get("assets_ticks_applied_total", 0) > a0), None)
    uend = fam(mend, "assets_ticks_unbound_total")
    if first is None:
        lines.append("INFO assets_ticks_applied_total did not rise over the row (member %s): unbound delta %d printed, not judged"
                     % (spec["ae_member"], uend - u0))
    else:
        u1 = first[2].get("assets_ticks_unbound_total", 0)
        lines.append("INFO assets_ticks_unbound_total before the first applied tick: +%d (printed, not judged)" % (u1 - u0))
        check(uend - u1 == 0, "assets_ticks_unbound_total delta 0 from the first applied tick to the end: %d" % (uend - u1))
    lines.append("INFO core member %s deltas: applied %d stale %d invalid %d epoch_drop %d unbound %d" % (
        spec["ae_member"], d("assets_ticks_applied_total"), d("assets_ticks_stale_total"), d("assets_ticks_invalid_total"),
        d("assets_ticks_epoch_drop_total"), d("assets_ticks_unbound_total")))
    for p, (s, e, up) in sorted(fwd.items()):
        dd = lambda **kw: fam(e, "forwarder_tick_offered_total", **kw) - fam(s, "forwarder_tick_offered_total", **kw)
        off, ro, rt = dd(market="1"), dd(market="1", edge="reoffer"), dd(market="1", edge="retry")
        bound = int(up * 10) + 1 + ro + rt
        check(off <= bound, "fwd %s offered market 1 delta %d <= row %.1f s x 10 + 1 + reoffers %d + retries %d = %d" % (
            p, off, up, ro, rt, bound))
        for m in range(2, 6):
            st = e.get('forwarder_book_state{market="%d"}' % m)
            mal = fam(e, "forwarder_book_chain_breaks_total", market=str(m), cause="malformed") - \
                fam(s, "forwarder_book_chain_breaks_total", market=str(m), cause="malformed")
            frows = snapshots(load_sampler(spec["sampler"]), spec["fwd_names"][p])
            mx = max([r[2].get('forwarder_book_state{market="%d"}' % m, 0) for r in frows] or [0])
            check(st in (0, 1) and mx != 2 and mal == 0,
                  "fwd %s market %d state %s (sampled max %d), malformed breaks delta %d" % (p, m, st, mx, mal))
        check(e.get("forwarder_ticks_ready") == 1, "fwd %s forwarder_ticks_ready 1: %s" % (p, e.get("forwarder_ticks_ready")))
        oos = e.get('forwarder_unknown_template_total{cluster="core",template="22"}')
        if oos:
            lines.append("INFO fwd %s forwarder_unknown_template_total{cluster=\"core\",template=\"22\"} %d (a core frame the "
                         "S12 forwarder does not know after a core (re-)registration; out of S13 scope, not an ME series)"
                         % (p, oos))
    for line in lines:
        print(line)
    print("O-T1 PASS" if not fails else "O-T1 FAIL: " + "; ".join(fails))
    return 0 if not fails else 1


def new_leader_lines(log):
    out = []
    for line in open(log, errors="replace"):
        m = re.match(r"INFO \[(\d+)\] ME new leader: term (-?\d+) -> (-?\d+)", line)
        if m:
            out.append((int(m.group(1)), line.strip()))
    return out


def delay(log, sampler, name, market, after, bound, version_positive, which):
    lines = [x for x in new_leader_lines(log) if x[0] >= after]
    if not lines:
        print("DELAY FAIL no \"ME new leader\" line in %s after mono %d ms" % (log, after))
        return 1
    t, text = lines[0] if which == "first" else lines[-1]
    snaps = snapshots(load_sampler(sampler), name)
    k22 = 'forwarder_book_frames_total{market="%s",template="22"}' % market
    kst = 'forwarder_book_state{market="%s"}' % market
    kver = 'forwarder_book_version{market="%s"}' % market
    before = [s for s in snaps if s[1] <= t]
    base = before[-1][2].get(k22, 0) if before else 0
    prev_end = before[-1][1] if before else t
    for t0, t1, v in snaps:
        if t0 < t:
            continue
        ok = v.get(k22, 0) > base and v.get(kst) == 1 and (not version_positive or v.get(kver, 0) > 0)
        if ok:
            up, lo = t1 - t, max(0, prev_end - t)
            verdict = "PASS" if up <= bound else "FAIL"
            print("DELAY %s market %s OK with template 22 +%d after the line (%s): <= %d ms (> %d ms), bound %d ms, version %d"
                  " line_mono=%d" % (verdict, market, v.get(k22, 0) - base, text, up, lo, bound, v.get(kver, 0), t))
            return 0 if up <= bound else 1
        prev_end = t1
    last = snaps[-1] if snaps else (t, t, {})
    print("DELAY FAIL market %s: no BookSnapshot (template 22 +1 with the ladder OK) after the line (%s) in the %d ms sampled "
          "after it (22 count %d -> %d, state %s) line_mono=%d" % (market, text, last[1] - t, base, last[2].get(k22, 0),
                                                                    last[2].get(kst), t))
    return 1


def nooffer(sampler, name, market):
    kst = 'forwarder_book_state{market="%s"}' % market
    pre = 'forwarder_tick_offered_total{market="%s"' % market
    snaps = snapshots(load_sampler(sampler), name)
    off = lambda v: sum(x for k, x in v.items() if k.startswith(pre))
    broken = sum(1 for _, _, v in snaps if v.get(kst) == 2)
    judged, bad, run, longest = 0, [], 0, 0
    for (a0, a1, va), (b0, b1, vb) in zip(snaps, snaps[1:]):
        if va.get(kst) == 2 and vb.get(kst) == 2:
            judged += 1
            run += 1
            longest = max(longest, run)
            if off(vb) > off(va):
                bad.append("offered %d -> %d in the window ending mono %d" % (off(va), off(vb), b1))
        else:
            run = 0
    if judged == 0:
        print("NOOFFER FAIL market %s: no sample window with the ladder BROKEN at both ends (BROKEN samples %d of %d): "
              "the property was not exercised" % (market, broken, len(snaps)))
        return 1
    if bad:
        print("NOOFFER FAIL market %s: %d of %d BROKEN windows saw an offer: %s" % (market, len(bad), judged, "; ".join(bad[:5])))
        return 1
    print("NOOFFER PASS market %s: offered counter flat across %d sample windows with the ladder BROKEN at both ends "
          "(BROKEN samples %d of %d, longest run %d windows ~ %d ms)" % (market, judged, broken, len(snaps), longest, longest * 100))
    return 0


def series(sampler, name, key, after):
    for t0, t1, v in snapshots(load_sampler(sampler), name):
        if t1 >= after and key in v:
            print("%d %s" % (t1, v[key]))
    return 0


def rise(sampler, name, t, keys):
    snaps = snapshots(load_sampler(sampler), name)
    val = lambda v, k: sum(x for s, x in v.items() if s.startswith(k)) if k.endswith("{") else v.get(k, 0)
    before = [x for x in snaps if x[1] < t]
    for k in keys:
        b = val(before[-1][2], k) if before else 0
        e = val(snaps[-1][2], k) if snaps else 0
        print("RISE %s before=%d end=%d" % (k, b, e))
    return 0


def logline(log, rx, after):
    r = re.compile(rx)
    for line in open(log, errors="replace"):
        m = re.match(r"\w+ \[(\d+)\] ", line)
        if m and int(m.group(1)) >= after and r.search(line):
            print(m.group(1))
            return 0
    print("none")
    return 1


def changes(sampler):
    last = {}
    for line in open(sampler):
        p = line.split()
        if len(p) != 5:
            continue
        k = (p[2], p[3])
        if last.get(k) != p[4]:
            last[k] = p[4]
            sys.stdout.write(line)
    return 0


def main(a):
    if a[0] == "sample":
        sample(a[1], float(a[2]), a[3:])
    elif a[0] == "oracle":
        return oracle(a[1])
    elif a[0] == "delay":
        which = a[a.index("--which") + 1] if "--which" in a else "first"
        return delay(a[1], a[2], a[3], a[4], int(a[5]), int(a[6]), "--version-positive" in a, which)
    elif a[0] == "series":
        return series(a[1], a[2], a[3], int(a[4]) if len(a) > 4 else 0)
    elif a[0] == "rise":
        return rise(a[1], a[2], int(a[3]), a[4:])
    elif a[0] == "logline":
        return logline(a[1], a[2], int(a[3]))
    elif a[0] == "nooffer":
        return nooffer(a[1], a[2], a[3])
    elif a[0] == "changes":
        return changes(a[1])
    elif a[0] == "mono":
        print(mono_ms())
    else:
        print("unknown command %s" % a[0])
        return 64
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
