#include "cluster_tools/domain/bridge/Stager.h"

#include "cluster_tools/archive/FramePeek.h"
#include "cluster_tools/domain/bridge/TestSeams.h"

#include <cstring>

namespace cluster_tools { namespace domain { namespace bridge {

using test_seams::Rule;
using test_seams::disabled;

std::size_t Stager::cap() const noexcept
{
    return disabled(Rule::CAP_OFF) ? 2 * CAP : CAP;
}

ForwardResult Stager::forward(const std::uint8_t *record, std::size_t length, std::uint16_t templateId,
    const OfferedRecord &meta, FrameOutput &out) noexcept
{
    ForwardResult result;
    if (archive::TEMPLATE_TRADE == templateId)
    {
        if (TRADE_RECORD_LENGTH != length)
        {
            result.step = StepResult::ENCODER_FAILED; // the gate admits a trade only at 117 bytes
            return result;
        }
        if (m_size >= cap())
        {
            // before staging a trade when the cap is staged (a back-pressured cap flush): flush first
            result.step = flush(out);
            if (StepResult::OK != result.step)
            {
                return result;
            }
        }
        StagedTrade &t = m_trades[m_size];
        // RED shape STAGE_TRUNCATES: the copy loses its last byte
        std::memcpy(t.record, record, disabled(Rule::STAGE_TRUNCATES) ? TRADE_RECORD_LENGTH - 1 : TRADE_RECORD_LENGTH);
        if (disabled(Rule::STAGE_TRUNCATES))
        {
            t.record[TRADE_RECORD_LENGTH - 1] = 0;
        }
        t.seq = meta.seq;
        t.endPosition = meta.endPosition;
        t.source = meta.source;
        t.keyIndex = meta.keyIndex;
        m_meta[m_size] = meta;
        ++m_size;
        if (m_size == cap())
        {
            result.flush = flush(out); // after staging the 64th: at once (back-pressure keeps the 64)
        }
        return result;
    }
    if (!empty() && !disabled(Rule::FLUSH_BEFORE_NON_TRADE_OFF))
    {
        result.step = flush(out); // before any non-trade record (back-pressure: the record is redelivered)
        if (StepResult::OK != result.step)
        {
            return result;
        }
    }
    const std::size_t n = m_encoder.encodeRecord(record, length, m_out, sizeof(m_out));
    if (0 == n)
    {
        result.step = StepResult::ENCODER_FAILED;
        return result;
    }
    result.step = out.emit(m_out, n, frameTemplateFor(templateId), &meta, 1);
    return result;
}

StepResult Stager::flush(FrameOutput &out) noexcept
{
    if (0 == m_size)
    {
        m_pending = false;
        return StepResult::OK;
    }
    std::size_t n = 0;
    std::uint16_t moneyTemplate = 0;
    if (1 == m_size)
    {
        n = m_encoder.encodeRecord(m_trades[0].record, TRADE_RECORD_LENGTH, m_out, sizeof(m_out));
        moneyTemplate = T_ME_JOURNAL_TRADE;
    }
    else
    {
        n = m_encoder.encodeTradeBatch(m_trades, m_size, m_out, sizeof(m_out));
        moneyTemplate = T_ME_JOURNAL_TRADE_BATCH;
    }
    if (0 == n)
    {
        return StepResult::ENCODER_FAILED;
    }
    const StepResult r = out.emit(m_out, n, moneyTemplate, m_meta, m_size);
    if (StepResult::OK == r)
    {
        m_size = 0;
        m_pending = false;
        return r;
    }
    if (disabled(Rule::BP_DROPS) && !isFatal(r))
    {
        m_size = 0; // RED shape: back-pressure drops the staged batch
        m_pending = false;
        return StepResult::OK;
    }
    m_pending = true;
    return r;
}

std::size_t Stager::drop() noexcept
{
    const std::size_t n = m_size;
    m_size = 0;
    m_pending = false;
    return n;
}

}}}
