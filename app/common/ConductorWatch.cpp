#include "common/ConductorWatch.h"

#include <chrono>

#include <Aeron.h>

namespace cluster_tools { namespace app { namespace common {

namespace {

std::int64_t systemEpochMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// The client's CountersReader as HeartbeatCounters (shared memory reads, no conductor involved).
class AeronHeartbeatCounters final : public HeartbeatCounters
{
public:
    explicit AeronHeartbeatCounters(::aeron::Aeron &aeron) : m_reader(aeron.countersReader()) {}

    std::int32_t maxCounterId() const override { return m_reader.maxCounterId(); }
    std::int32_t recordState(std::int32_t id) const override
    {
        return m_reader.metaDataBuffer().getInt32Volatile(::aeron::concurrent::CountersReader::metadataOffset(id));
    }
    std::int32_t typeId(std::int32_t id) const override
    {
        return m_reader.metaDataBuffer().getInt32(::aeron::concurrent::CountersReader::metadataOffset(id) +
            ::aeron::concurrent::CountersReader::TYPE_ID_OFFSET);
    }
    std::int64_t key(std::int32_t id) const override
    {
        return m_reader.metaDataBuffer().getInt64(::aeron::concurrent::CountersReader::metadataOffset(id) +
            ::aeron::concurrent::CountersReader::KEY_OFFSET);
    }
    std::int64_t value(std::int32_t id) const override { return m_reader.getCounterValue(id); }

private:
    ::aeron::concurrent::CountersReader &m_reader;
};

static_assert(COUNTER_RECORD_ALLOCATED == ::aeron::concurrent::CountersReader::RECORD_ALLOCATED);

}

ConductorWatch::ConductorWatch(std::shared_ptr<::aeron::Aeron> aeron, std::int64_t timeoutMs, EpochMsClock clock,
    bool revalidate) :
    m_aeron(std::move(aeron)), m_timeoutMs(timeoutMs), m_clock(clock ? std::move(clock) : EpochMsClock(&systemEpochMs)),
    m_counters(std::make_unique<AeronHeartbeatCounters>(*m_aeron)),
    m_core(std::make_unique<ConductorWatchCore>(*m_counters, m_aeron->clientId(), timeoutMs, revalidate))
{
}

ConductorWatch::~ConductorWatch() = default;

bool ConductorWatch::check(std::string &why)
{
    return m_core->check(m_clock(), why);
}

}}}
