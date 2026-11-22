#include "genesis/Json.h"

#include <limits>

namespace cluster_tools { namespace app { namespace genesis { namespace json {

const Value *Value::find(const std::string &key) const
{
    for (const auto &member : members)
    {
        if (member.first == key)
        {
            return &member.second;
        }
    }
    return nullptr;
}

const char *kindName(Value::Kind kind) noexcept
{
    switch (kind)
    {
        case Value::Kind::NUL: return "null";
        case Value::Kind::BOOL: return "a boolean";
        case Value::Kind::INT: return "an integer";
        case Value::Kind::STRING: return "a string";
        case Value::Kind::ARRAY: return "an array";
        case Value::Kind::OBJECT: return "an object";
    }
    return "?";
}

namespace {

constexpr int MAX_DEPTH = 16;

class Parser
{
public:
    explicit Parser(const std::string &text) : m_text(text) {}

    ParseResult run()
    {
        ParseResult result;
        skipSpace();
        if (!value(result.value, 0))
        {
            result.error = m_error;
            return result;
        }
        skipSpace();
        if (m_pos != m_text.size())
        {
            result.error = at(m_pos, "unexpected content after the document");
        }
        return result;
    }

private:
    std::string at(std::size_t offset, const std::string &what) const
    {
        return what + " at byte " + std::to_string(offset);
    }

    bool fail(const std::string &what)
    {
        if (m_error.empty())
        {
            m_error = at(m_pos, what);
        }
        return false;
    }

    void skipSpace()
    {
        while (m_pos < m_text.size() &&
            (' ' == m_text[m_pos] || '\t' == m_text[m_pos] || '\n' == m_text[m_pos] || '\r' == m_text[m_pos]))
        {
            m_pos++;
        }
    }

    bool literal(const char *word)
    {
        const std::string w(word);
        if (m_text.compare(m_pos, w.size(), w) != 0)
        {
            return fail("invalid literal");
        }
        m_pos += w.size();
        return true;
    }

    bool value(Value &out, int depth)
    {
        if (depth > MAX_DEPTH)
        {
            return fail("nesting deeper than " + std::to_string(MAX_DEPTH));
        }
        if (m_pos >= m_text.size())
        {
            return fail("unexpected end of document");
        }
        out.offset = m_pos;
        const char c = m_text[m_pos];
        if ('{' == c)
        {
            return object(out, depth);
        }
        if ('[' == c)
        {
            return array(out, depth);
        }
        if ('"' == c)
        {
            out.kind = Value::Kind::STRING;
            return string(out.text);
        }
        if ('t' == c)
        {
            out.kind = Value::Kind::BOOL;
            out.boolean = true;
            return literal("true");
        }
        if ('f' == c)
        {
            out.kind = Value::Kind::BOOL;
            out.boolean = false;
            return literal("false");
        }
        if ('n' == c)
        {
            out.kind = Value::Kind::NUL;
            return literal("null");
        }
        if ('-' == c || (c >= '0' && c <= '9'))
        {
            out.kind = Value::Kind::INT;
            return integer(out.integer);
        }
        return fail(std::string("unexpected character '") + c + "'");
    }

    bool integer(std::int64_t &out)
    {
        const std::size_t start = m_pos;
        bool negative = false;
        if ('-' == m_text[m_pos])
        {
            negative = true;
            m_pos++;
        }
        if (m_pos >= m_text.size() || m_text[m_pos] < '0' || m_text[m_pos] > '9')
        {
            return fail("a number needs a digit");
        }
        if ('0' == m_text[m_pos] && m_pos + 1 < m_text.size() && m_text[m_pos + 1] >= '0' && m_text[m_pos + 1] <= '9')
        {
            return fail("a number with a leading zero");
        }
        // accumulate as a negative value so INT64_MIN is representable
        std::int64_t acc = 0;
        while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9')
        {
            const int digit = m_text[m_pos] - '0';
            if (acc < (std::numeric_limits<std::int64_t>::min() + digit) / 10)
            {
                m_pos = start;
                return fail("an integer outside int64");
            }
            acc = acc * 10 - digit;
            m_pos++;
        }
        if (m_pos < m_text.size() && ('.' == m_text[m_pos] || 'e' == m_text[m_pos] || 'E' == m_text[m_pos]))
        {
            return fail("a number that is not an integer");
        }
        if (negative)
        {
            if (0 == acc)
            {
                m_pos = start;
                return fail("-0 is not an integer here");
            }
            out = acc;
            return true;
        }
        if (acc == std::numeric_limits<std::int64_t>::min())
        {
            m_pos = start;
            return fail("an integer outside int64");
        }
        out = -acc;
        return true;
    }

    bool string(std::string &out)
    {
        m_pos++; // the opening quote
        while (m_pos < m_text.size())
        {
            const char c = m_text[m_pos];
            if ('"' == c)
            {
                m_pos++;
                return true;
            }
            if ('\\' == c)
            {
                if (m_pos + 1 >= m_text.size())
                {
                    return fail("unterminated escape");
                }
                const char e = m_text[m_pos + 1];
                switch (e)
                {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    default: return fail(std::string("unsupported escape \\") + e);
                }
                m_pos += 2;
                continue;
            }
            if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e)
            {
                return fail("a string holds a control or non-ASCII byte");
            }
            out += c;
            m_pos++;
        }
        return fail("unterminated string");
    }

    bool array(Value &out, int depth)
    {
        out.kind = Value::Kind::ARRAY;
        m_pos++;
        skipSpace();
        if (m_pos < m_text.size() && ']' == m_text[m_pos])
        {
            m_pos++;
            return true;
        }
        for (;;)
        {
            skipSpace();
            Value item;
            if (!value(item, depth + 1))
            {
                return false;
            }
            out.items.push_back(std::move(item));
            skipSpace();
            if (m_pos >= m_text.size())
            {
                return fail("unterminated array");
            }
            if (',' == m_text[m_pos])
            {
                m_pos++;
                continue;
            }
            if (']' == m_text[m_pos])
            {
                m_pos++;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    bool object(Value &out, int depth)
    {
        out.kind = Value::Kind::OBJECT;
        m_pos++;
        skipSpace();
        if (m_pos < m_text.size() && '}' == m_text[m_pos])
        {
            m_pos++;
            return true;
        }
        for (;;)
        {
            skipSpace();
            if (m_pos >= m_text.size() || '"' != m_text[m_pos])
            {
                return fail("expected a key");
            }
            const std::size_t keyAt = m_pos;
            std::string key;
            if (!string(key))
            {
                return false;
            }
            if (nullptr != out.find(key))
            {
                m_pos = keyAt;
                return fail("duplicate key \"" + key + "\"");
            }
            skipSpace();
            if (m_pos >= m_text.size() || ':' != m_text[m_pos])
            {
                return fail("expected ':'");
            }
            m_pos++;
            skipSpace();
            Value member;
            if (!value(member, depth + 1))
            {
                return false;
            }
            out.members.emplace_back(std::move(key), std::move(member));
            skipSpace();
            if (m_pos >= m_text.size())
            {
                return fail("unterminated object");
            }
            if (',' == m_text[m_pos])
            {
                m_pos++;
                continue;
            }
            if ('}' == m_text[m_pos])
            {
                m_pos++;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    const std::string &m_text;
    std::size_t m_pos = 0;
    std::string m_error;
};

}

ParseResult parse(const std::string &text)
{
    return Parser(text).run();
}

}}}}
