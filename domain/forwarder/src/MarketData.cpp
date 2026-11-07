#include "cluster_tools/domain/forwarder/MarketData.h"

#include "cluster_tools/domain/forwarder/TestSeams.h"

namespace cluster_tools { namespace domain { namespace forwarder {

namespace seams = test_seams;

const char *malformedName(Malformed m) noexcept
{
    switch (m)
    {
        case Malformed::NONE: return "none";
        case Malformed::TEMPLATE: return "template";
        case Malformed::HEADER_BLOCK: return "header_block";
        case Malformed::GROUP_BLOCK: return "group_block";
        case Malformed::COUNT: return "count";
        case Malformed::LENGTH: return "length";
        case Malformed::VERSION: return "version";
        case Malformed::ENUM: return "enum";
        case Malformed::VALUE: return "value";
    }
    return "?";
}

Malformed validateBookFrame(const BookFrame &f) noexcept
{
    const std::uint16_t blockMin = f.snapshot ? SNAPSHOT_BLOCK_MIN : DELTA_BLOCK_MIN;
    const std::uint16_t entryMin = f.snapshot ? SNAPSHOT_ENTRY_MIN : DELTA_ENTRY_MIN;
    const unsigned groups = f.snapshot ? 2 : 1;
    const std::uint16_t countCap = f.snapshot ? LADDER_LEVELS : DELTA_MAX_CHANGES;

    if (f.headerBlockLength < blockMin && !seams::disabled(seams::Rule::STITCH_HEADER_BLOCK))
    {
        return Malformed::HEADER_BLOCK;
    }
    if (!seams::disabled(seams::Rule::STITCH_GROUP_BLOCK_LENGTH))
    {
        for (unsigned g = 0; g < groups; g++)
        {
            // a group header the frame was too short to hold reads as 0 / 0: that is LENGTH below, not a stated block
            const bool headerUnread = !f.lengthOk && 0 == f.groupBlockLength[g] && 0 == f.statedCount[g];
            if (f.groupBlockLength[g] < entryMin && !headerUnread)
            {
                return Malformed::GROUP_BLOCK;
            }
        }
    }
    if (!seams::disabled(seams::Rule::STITCH_COUNT_CAP))
    {
        for (unsigned g = 0; g < groups; g++)
        {
            if (f.statedCount[g] > countCap)
            {
                return Malformed::COUNT;
            }
        }
    }
    if (!f.lengthOk && !seams::disabled(seams::Rule::STITCH_FRAME_LENGTH))
    {
        return Malformed::LENGTH;
    }

    if (f.snapshot)
    {
        if (f.bookVersion < 0)
        {
            return Malformed::VERSION;
        }
        // rule U (4.2): version 0 with two empty groups is "no versioned state", not malformed; the seam makes it so
        const bool empty = 0 == f.nBids && 0 == f.nAsks;
        if (0 == f.bookVersion && (!empty || seams::disabled(seams::Rule::STITCH_UNVERSIONED_EMPTY)))
        {
            return Malformed::VERSION;
        }
        if (!seams::disabled(seams::Rule::STITCH_VALUE_CHECK))
        {
            for (std::uint16_t i = 0; i < f.nBids; i++)
            {
                if (f.bids[i].price <= 0 || f.bids[i].quantity <= 0 || f.bids[i].orderCount <= 0)
                {
                    return Malformed::VALUE;
                }
            }
            for (std::uint16_t i = 0; i < f.nAsks; i++)
            {
                if (f.asks[i].price <= 0 || f.asks[i].quantity <= 0 || f.asks[i].orderCount <= 0)
                {
                    return Malformed::VALUE;
                }
            }
        }
        return Malformed::NONE;
    }

    // 25: RC-N3, bookVersion above fromVersion keeps rule 2 from lowering the ladder's version
    if (!seams::disabled(seams::Rule::STITCH_DELTA_VERSION) &&
        (f.bookVersion <= 0 || f.fromVersion < 0 || f.bookVersion <= f.fromVersion))
    {
        return Malformed::VERSION;
    }
    if (!seams::disabled(seams::Rule::STITCH_RAW_ENUM_CHECK))
    {
        for (std::uint16_t i = 0; i < f.nChanges; i++)
        {
            if (f.changes[i].side > BOOK_SIDE_MAX || f.changes[i].updateType > BOOK_UPDATE_MAX)
            {
                return Malformed::ENUM;
            }
        }
    }
    if (!seams::disabled(seams::Rule::STITCH_VALUE_CHECK))
    {
        for (std::uint16_t i = 0; i < f.nChanges; i++)
        {
            const BookChange &c = f.changes[i];
            const bool del = BOOK_UPDATE_DELETE == c.updateType;
            if (c.price <= 0 || (!del && (c.quantity <= 0 || c.orderCount <= 0)))
            {
                return Malformed::VALUE;
            }
        }
    }
    return Malformed::NONE;
}

Malformed validateTradeFrame(const TradeFrame &f) noexcept
{
    if (f.headerBlockLength < TRADE_BLOCK_MIN && !seams::disabled(seams::Rule::STITCH_HEADER_BLOCK))
    {
        return Malformed::HEADER_BLOCK;
    }
    if (f.entryBlockLength < TRADE_ENTRY_MIN && !seams::disabled(seams::Rule::STITCH_GROUP_BLOCK_LENGTH))
    {
        return Malformed::GROUP_BLOCK;
    }
    if (!f.lengthOk && !seams::disabled(seams::Rule::STITCH_FRAME_LENGTH))
    {
        return Malformed::LENGTH;
    }
    if (f.entryInvalid && !seams::disabled(seams::Rule::STITCH_TRADE_VALIDATE))
    {
        return Malformed::VALUE;
    }
    return Malformed::NONE;
}

}}}
