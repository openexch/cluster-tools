#include "cluster_tools/domain/bridge/Verdicts.h"

#include "cluster_tools/domain/bridge/TestSeams.h"

#include <algorithm>

namespace cluster_tools { namespace domain { namespace bridge {

using test_seams::Rule;
using test_seams::disabled;

const char *decisionRowName(DecisionRow row) noexcept
{
    switch (row)
    {
        case DecisionRow::UNDECIDED: return "undecided";
        case DecisionRow::NO_REACHABLE: return "no_reachable_source";
        case DecisionRow::ALL_BEHIND: return "all_behind";
        case DecisionRow::HOLE_GRACE: return "hole_grace";
        case DecisionRow::GAP: return "gap";
        case DecisionRow::EPOCH: return "epoch_mismatch";
        case DecisionRow::SCHEMA: return "source_schema";
    }
    return "?";
}

void VerdictTable::atE(std::int64_t e) noexcept
{
    if (e != m_e)
    {
        clearDynamic();
        m_e = e;
    }
}

bool VerdictTable::set(std::size_t source, const Verdict &v, TimePoint now) noexcept
{
    Verdict &slot = m_v[source];
    if (!m_hasFirst && VerdictKind::REFUSED_STATIC != v.kind)
    {
        m_hasFirst = true;
        m_firstAt = now;
    }
    if (slot.kind == v.kind && slot.seq == v.seq && slot.epoch == v.epoch && slot.bad == v.bad)
    {
        return false;
    }
    slot = v;
    slot.since = now;
    return true;
}

void VerdictTable::clearDynamic() noexcept
{
    for (std::size_t i = 0; i < m_sources; ++i)
    {
        if (VerdictKind::REFUSED_STATIC != m_v[i].kind)
        {
            m_v[i] = Verdict{};
        }
    }
    m_hasFirst = false;
}

bool VerdictTable::anyUnknown() const noexcept
{
    for (std::size_t i = 0; i < m_sources; ++i)
    {
        if (VerdictKind::UNKNOWN == m_v[i].kind)
        {
            return true;
        }
    }
    return false;
}

bool VerdictTable::all(VerdictKind kind) const noexcept
{
    for (std::size_t i = 0; i < m_sources; ++i)
    {
        if (kind != m_v[i].kind)
        {
            return false;
        }
    }
    return true;
}

namespace {

bool inR(VerdictKind k) noexcept
{
    return VerdictKind::HOLED == k || VerdictKind::BAD == k || VerdictKind::FOREIGN_EPOCH == k || VerdictKind::BEHIND == k;
}

VerdictKind effective(VerdictKind k) noexcept
{
    // RED shape UNREACHABLE_AS_HOLE: an unreachable source counts as a holed one
    if (VerdictKind::UNREACHABLE == k && disabled(Rule::UNREACHABLE_AS_HOLE))
    {
        return VerdictKind::HOLED;
    }
    return k;
}

}

Decision decide(const VerdictTable &t, bool graceExpired, std::size_t lastForwardSource) noexcept
{
    Decision d;
    const std::size_t n = t.sources();
    if (t.anyUnknown())
    {
        return d; // UNDECIDED: the rotation is not over
    }
    bool anyR = false, allRBehind = true, strong = false, lagging = false;
    std::size_t firstBehind = NO_SOURCE, firstNotStatic = NO_SOURCE;
    for (std::size_t i = 0; i < n; ++i)
    {
        const VerdictKind k = effective(t.at(i).kind);
        if (VerdictKind::REFUSED_STATIC != k && NO_SOURCE == firstNotStatic)
        {
            firstNotStatic = i;
        }
        if (inR(k))
        {
            anyR = true;
            if (VerdictKind::BEHIND != k)
            {
                allRBehind = false;
                strong = true;
            }
            else if (NO_SOURCE == firstBehind)
            {
                firstBehind = i;
            }
        }
        if (VerdictKind::BEHIND == k || VerdictKind::UNREACHABLE == k)
        {
            lagging = true;
        }
    }
    // Row 1: every source unreachable or statically refused: a reported degraded state, never a halt.
    if (!anyR)
    {
        d.row = DecisionRow::NO_REACHABLE;
        d.target = firstNotStatic;
        return d;
    }
    // Row 2: every source in R is behind (a quiet feed, or every node lagging): park on the last live source.
    if (allRBehind)
    {
        d.row = DecisionRow::ALL_BEHIND;
        const bool lastIsBehind = NO_SOURCE != lastForwardSource && lastForwardSource < n &&
            VerdictKind::BEHIND == t.at(lastForwardSource).kind;
        d.target = (lastIsBehind && !disabled(Rule::BEHIND_PARK_FIRST_CONFIGURED)) ? lastForwardSource : firstBehind;
        return d;
    }
    // Row 3: a holed, bad or foreign source while another one lags: wait holeGrace before any halt.
    if (strong && lagging && !graceExpired && !disabled(Rule::GRACE_OFF))
    {
        d.row = DecisionRow::HOLE_GRACE;
        d.target = firstBehind;
        return d;
    }
    // Rows 4-6, with every BEHIND source counted as HOLED without a seen and the UNREACHABLE ones left out.
    bool allHoled = true, anyHoled = false, anyBadOrBehindOrHoled = true;
    std::int64_t minSeen = 0, maxSeen = 0;
    for (std::size_t i = 0; i < n; ++i)
    {
        const Verdict &v = t.at(i);
        const VerdictKind k = effective(v.kind);
        if (!inR(k))
        {
            continue;
        }
        if (VerdictKind::HOLED == k)
        {
            const std::int64_t seen = VerdictKind::HOLED == v.kind ? v.seq : t.e() + 1;
            if (!anyHoled || seen < minSeen)
            {
                minSeen = seen;
            }
            if (!anyHoled || seen > maxSeen)
            {
                maxSeen = seen;
            }
            anyHoled = true;
        }
        else if (VerdictKind::BEHIND != k)
        {
            allHoled = false;
            if (VerdictKind::FOREIGN_EPOCH == k)
            {
                anyBadOrBehindOrHoled = false;
            }
        }
    }
    if (allHoled && anyHoled)
    {
        d.row = DecisionRow::GAP;
        d.seen = disabled(Rule::GAP_SEEN_MAX) ? maxSeen : minSeen;
        return d;
    }
    // Row 5: EVERY CONFIGURED source is foreign at e (review FIX-1); the RED shape counts only the sources in R.
    bool everyForeign = true;
    std::size_t firstForeign = NO_SOURCE;
    for (std::size_t i = 0; i < n; ++i)
    {
        const VerdictKind k = effective(t.at(i).kind);
        if (VerdictKind::FOREIGN_EPOCH == k)
        {
            if (NO_SOURCE == firstForeign)
            {
                firstForeign = i;
            }
            continue;
        }
        if (disabled(Rule::EPOCH_REACHABLE_ONLY) && !inR(k))
        {
            continue;
        }
        everyForeign = false;
    }
    if (everyForeign && NO_SOURCE != firstForeign)
    {
        d.row = DecisionRow::EPOCH;
        d.seen = t.at(firstForeign).seq;
        d.epochSeen = t.at(firstForeign).epoch;
        return d;
    }
    // RED shape Q2_GAP: a mix of holed and bad sources halts GAP.
    if (disabled(Rule::Q2_GAP) && anyHoled && anyBadOrBehindOrHoled)
    {
        d.row = DecisionRow::GAP;
        d.seen = minSeen;
        return d;
    }
    d.row = DecisionRow::SCHEMA; // row 6 (Q2 decided: a mixed rotation halts SOURCE_SCHEMA)
    return d;
}

}}}
