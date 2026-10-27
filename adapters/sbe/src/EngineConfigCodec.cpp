#include "cluster_tools/adapters/sbe/EngineConfigCodec.h"

#include <string>

#include "com_match_infrastructure_generated/MessageHeader.h"
#include "com_match_infrastructure_generated/GroupSizeEncoding.h"
#include "com_match_infrastructure_generated/EngineImpl.h"
#include "com_match_infrastructure_generated/EngineConfig.h"

namespace mm = com::match::infrastructure::generated;

namespace cluster_tools { namespace sbe_adapter {

namespace
{

// The group dimension is a uint16: more markets than that cannot ride this wire at
// all. A WIRE limit, not a domain rule — the engine can never even see the overflow.
constexpr std::size_t MAX_WIRE_MARKETS = 65535;

}

EngineConfigWireInfo engineConfigWireInfo() noexcept
{
    return EngineConfigWireInfo{
        mm::EngineConfig::sbeSchemaId(),
        mm::EngineConfig::sbeSchemaVersion(),
        mm::EngineConfig::sbeTemplateId() };
}

ports::Result<std::vector<std::uint8_t>> encodeEngineConfig(const domain::EngineConfig &config)
{
    using ResultType = ports::Result<std::vector<std::uint8_t>>;

    if (domain::EngineImpl::ARRAY != config.impl && domain::EngineImpl::DIRECT != config.impl)
    {
        // Everything else garbage still encodes (validation is the domain's job and
        // the wire carries any uint32); an unknown impl has NO byte to write.
        return ResultType::failure("encode: impl is unknown — validate before encoding");
    }
    if (config.markets.size() > MAX_WIRE_MARKETS)
    {
        return ResultType::failure("encode: " + std::to_string(config.markets.size()) +
            " markets exceed the wire group's uint16 dimension (65535)");
    }

    try
    {
        std::vector<std::uint8_t> frame(
            mm::MessageHeader::encodedLength() + mm::EngineConfig::sbeBlockLength() +
            mm::GroupSizeEncoding::encodedLength() +
            config.markets.size() * mm::EngineConfig::Markets::sbeBlockLength());

        mm::EngineConfig encoder;
        encoder.wrapAndApplyHeader(reinterpret_cast<char *>(frame.data()), 0, frame.size())
            .configVersion(static_cast<std::uint32_t>(config.configVersion))
            .impl(domain::EngineImpl::ARRAY == config.impl
                ? mm::EngineImpl::ARRAY : mm::EngineImpl::DIRECT)
            .bookCapacity(static_cast<std::uint32_t>(config.bookCapacity))
            .maxMatchesPerOrder(static_cast<std::uint32_t>(config.maxMatchesPerOrder))
            .maxOrdersPerLevel(static_cast<std::uint32_t>(config.maxOrdersPerLevel))
            .behaviorVersion(static_cast<std::uint32_t>(config.behaviorVersion))
            .commandLedgerCapacity(static_cast<std::uint32_t>(config.commandLedgerCapacity))
            .genesisId(config.genesisId)
            .maxWireVersion(static_cast<std::uint16_t>(config.maxWireVersion));

        mm::EngineConfig::Markets &markets =
            encoder.marketsCount(static_cast<std::uint16_t>(config.markets.size()));
        for (const domain::MarketDef &market : config.markets)
        {
            markets.next()
                .marketId(market.marketId)
                .minPrice(market.minPrice)
                .maxPrice(market.maxPrice)
                .tickSize(market.tickSize);
            markets.putSymbol(market.symbol); // zero-pads to the wire's char[16]
        }

        frame.resize(mm::MessageHeader::encodedLength() + encoder.encodedLength());
        return ResultType::success(std::move(frame));
    }
    catch (const std::exception &e)
    {
        return ResultType::failure(std::string("encode: ") + e.what());
    }
    catch (...)
    {
        return ResultType::failure("encode: unknown exception");
    }
}

}}
