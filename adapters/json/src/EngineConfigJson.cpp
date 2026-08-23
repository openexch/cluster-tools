#include "cluster_tools/adapters/json/EngineConfigJson.h"

#include <cstdint>

namespace cluster_tools { namespace json_adapter {

namespace
{

using domain::EngineConfig;
using domain::EngineImpl;
using domain::MarketDef;

// Recursive-descent parser over the one known document shape. Exception-free: every
// step either advances or records the first error (position included) and fails fast.
struct Parser
{
    const std::string &text;
    std::size_t pos = 0;
    std::string error;

    explicit Parser(const std::string &textIn) : text(textIn)
    {
    }

    bool fail(const std::string &message)
    {
        if (error.empty())
        {
            error = message + " at offset " + std::to_string(pos);
        }
        return false;
    }

    bool atEnd() const
    {
        return pos >= text.size();
    }

    char peek() const
    {
        return text[pos];
    }

    void skipWhitespace()
    {
        while (!atEnd())
        {
            const char c = peek();
            if (' ' != c && '\t' != c && '\n' != c && '\r' != c)
            {
                break;
            }
            pos++;
        }
    }

    bool expect(char c, const char *what)
    {
        skipWhitespace();
        if (atEnd() || peek() != c)
        {
            return fail(std::string("expected ") + what);
        }
        pos++;
        return true;
    }

    bool parseString(std::string &out)
    {
        skipWhitespace();
        if (atEnd() || '"' != peek())
        {
            return fail("expected a string");
        }
        pos++;
        out.clear();
        while (true)
        {
            if (atEnd())
            {
                return fail("unterminated string");
            }
            const unsigned char c = static_cast<unsigned char>(text[pos]);
            if ('"' == c)
            {
                pos++;
                return true;
            }
            if (c < 0x20)
            {
                return fail("raw control character in string");
            }
            if ('\\' == c)
            {
                pos++;
                if (atEnd())
                {
                    return fail("unterminated escape");
                }
                const char e = text[pos];
                switch (e)
                {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u':
                        return fail("\\u escapes are not supported (fields on this wire are plain ASCII)");
                    default:
                        return fail("unknown escape");
                }
                pos++;
                continue;
            }
            out.push_back(static_cast<char>(c));
            pos++;
        }
    }

    // JSON integer, strictly: optional '-', digits, no leading zeros, and NOT a float —
    // every numeric field on this wire is integral (prices are raw fixed-point int64),
    // so '.', 'e' and 'E' are refused loudly rather than truncated silently.
    bool parseInt64(std::int64_t &out)
    {
        skipWhitespace();
        const std::size_t start = pos;
        bool negative = false;
        if (!atEnd() && '-' == peek())
        {
            negative = true;
            pos++;
        }
        if (atEnd() || peek() < '0' || peek() > '9')
        {
            pos = start;
            return fail("expected an integer");
        }
        if ('0' == peek() && pos + 1 < text.size() && text[pos + 1] >= '0' && text[pos + 1] <= '9')
        {
            return fail("leading zeros are not valid JSON");
        }
        // Accumulate as a negative number: INT64_MIN has no positive counterpart.
        std::int64_t value = 0;
        while (!atEnd() && peek() >= '0' && peek() <= '9')
        {
            const std::int64_t digit = peek() - '0';
            if (value < (INT64_MIN + digit) / 10)
            {
                return fail("integer does not fit int64");
            }
            value = value * 10 - digit;
            pos++;
        }
        if (!atEnd() && ('.' == peek() || 'e' == peek() || 'E' == peek()))
        {
            return fail("expected an integer, not a float (fixed-point fields are raw int64 values)");
        }
        if (!negative)
        {
            if (INT64_MIN == value)
            {
                return fail("integer does not fit int64");
            }
            value = -value;
        }
        out = value;
        return true;
    }

    bool parseMarket(MarketDef &market)
    {
        if (!expect('{', "'{' to open a market object"))
        {
            return false;
        }
        bool seenId = false, seenSymbol = false, seenMin = false, seenMax = false, seenTick = false;
        while (true)
        {
            std::string key;
            if (!parseString(key))
            {
                return fail("expected a market key");
            }
            if (!expect(':', "':' after the key"))
            {
                return false;
            }
            if ("marketId" == key)
            {
                if (seenId)
                {
                    return fail("duplicate key 'marketId'");
                }
                seenId = true;
                std::int64_t wide = 0;
                if (!parseInt64(wide))
                {
                    return false;
                }
                if (wide < INT32_MIN || wide > INT32_MAX)
                {
                    return fail("marketId does not fit int32");
                }
                market.marketId = static_cast<std::int32_t>(wide);
            }
            else if ("symbol" == key)
            {
                if (seenSymbol)
                {
                    return fail("duplicate key 'symbol'");
                }
                seenSymbol = true;
                if (!parseString(market.symbol))
                {
                    return false;
                }
            }
            else if ("minPrice" == key)
            {
                if (seenMin)
                {
                    return fail("duplicate key 'minPrice'");
                }
                seenMin = true;
                if (!parseInt64(market.minPrice))
                {
                    return false;
                }
            }
            else if ("maxPrice" == key)
            {
                if (seenMax)
                {
                    return fail("duplicate key 'maxPrice'");
                }
                seenMax = true;
                if (!parseInt64(market.maxPrice))
                {
                    return false;
                }
            }
            else if ("tickSize" == key)
            {
                if (seenTick)
                {
                    return fail("duplicate key 'tickSize'");
                }
                seenTick = true;
                if (!parseInt64(market.tickSize))
                {
                    return false;
                }
            }
            else
            {
                return fail("unknown market key '" + key + "'");
            }
            skipWhitespace();
            if (!atEnd() && ',' == peek())
            {
                pos++;
                continue;
            }
            break;
        }
        if (!expect('}', "',' or '}' in the market object"))
        {
            return false;
        }
        if (!seenId || !seenSymbol || !seenMin || !seenMax || !seenTick)
        {
            std::string missing;
            if (!seenId) missing += " marketId";
            if (!seenSymbol) missing += " symbol";
            if (!seenMin) missing += " minPrice";
            if (!seenMax) missing += " maxPrice";
            if (!seenTick) missing += " tickSize";
            return fail("market object is missing required key(s):" + missing);
        }
        return true;
    }

    bool parseMarkets(std::vector<MarketDef> &markets)
    {
        if (!expect('[', "'[' to open the markets array"))
        {
            return false;
        }
        skipWhitespace();
        if (!atEnd() && ']' == peek())
        {
            pos++; // an EMPTY market set is well-formed shape; the DOMAIN rejects it
            return true;
        }
        while (true)
        {
            MarketDef market;
            if (!parseMarket(market))
            {
                return false;
            }
            markets.push_back(market);
            skipWhitespace();
            if (!atEnd() && ',' == peek())
            {
                pos++;
                continue;
            }
            break;
        }
        return expect(']', "',' or ']' in the markets array");
    }

    bool parseImpl(EngineImpl &impl)
    {
        std::string name;
        if (!parseString(name))
        {
            return false;
        }
        if ("array" == name)
        {
            impl = EngineImpl::ARRAY;
            return true;
        }
        if ("direct" == name)
        {
            impl = EngineImpl::DIRECT;
            return true;
        }
        return fail("impl must be \"array\" or \"direct\" (was \"" + name + "\")");
    }

    bool parse(EngineConfig &config)
    {
        if (!expect('{', "'{' to open the engine-config object"))
        {
            return false;
        }
        bool seenVersion = false, seenImpl = false, seenCapacity = false,
            seenMatches = false, seenPerLevel = false, seenMarkets = false;
        while (true)
        {
            std::string key;
            if (!parseString(key))
            {
                return fail("expected a key");
            }
            if (!expect(':', "':' after the key"))
            {
                return false;
            }
            if ("configVersion" == key)
            {
                if (seenVersion)
                {
                    return fail("duplicate key 'configVersion'");
                }
                seenVersion = true;
                if (!parseInt64(config.configVersion))
                {
                    return false;
                }
            }
            else if ("impl" == key)
            {
                if (seenImpl)
                {
                    return fail("duplicate key 'impl'");
                }
                seenImpl = true;
                if (!parseImpl(config.impl))
                {
                    return false;
                }
            }
            else if ("bookCapacity" == key)
            {
                if (seenCapacity)
                {
                    return fail("duplicate key 'bookCapacity'");
                }
                seenCapacity = true;
                if (!parseInt64(config.bookCapacity))
                {
                    return false;
                }
            }
            else if ("maxMatchesPerOrder" == key)
            {
                if (seenMatches)
                {
                    return fail("duplicate key 'maxMatchesPerOrder'");
                }
                seenMatches = true;
                if (!parseInt64(config.maxMatchesPerOrder))
                {
                    return false;
                }
            }
            else if ("maxOrdersPerLevel" == key)
            {
                if (seenPerLevel)
                {
                    return fail("duplicate key 'maxOrdersPerLevel'");
                }
                seenPerLevel = true;
                if (!parseInt64(config.maxOrdersPerLevel))
                {
                    return false;
                }
            }
            else if ("markets" == key)
            {
                if (seenMarkets)
                {
                    return fail("duplicate key 'markets'");
                }
                seenMarkets = true;
                if (!parseMarkets(config.markets))
                {
                    return false;
                }
            }
            else
            {
                return fail("unknown key '" + key + "'");
            }
            skipWhitespace();
            if (!atEnd() && ',' == peek())
            {
                pos++;
                continue;
            }
            break;
        }
        if (!expect('}', "',' or '}' in the engine-config object"))
        {
            return false;
        }
        skipWhitespace();
        if (!atEnd())
        {
            return fail("unexpected content after the closing '}'");
        }
        if (!seenVersion || !seenImpl || !seenCapacity || !seenMatches || !seenPerLevel || !seenMarkets)
        {
            std::string missing;
            if (!seenVersion) missing += " configVersion";
            if (!seenImpl) missing += " impl";
            if (!seenCapacity) missing += " bookCapacity";
            if (!seenMatches) missing += " maxMatchesPerOrder";
            if (!seenPerLevel) missing += " maxOrdersPerLevel";
            if (!seenMarkets) missing += " markets";
            return fail("missing required key(s):" + missing);
        }
        return true;
    }
};

}

ports::Result<domain::EngineConfig> parseEngineConfig(const std::string &text)
{
    using ResultType = ports::Result<domain::EngineConfig>;

    Parser parser(text);
    EngineConfig config;
    if (!parser.parse(config))
    {
        return ResultType::failure("engine-config json: " + parser.error);
    }
    return ResultType::success(std::move(config));
}

}}
