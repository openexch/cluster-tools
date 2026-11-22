#ifndef CLUSTER_TOOLS_APP_GENESIS_JSON_H
#define CLUSTER_TOOLS_APP_GENESIS_JSON_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// A strict JSON reader for the genesis input file. The house style of adapters/json, as a small DOM because the
// genesis document has nested blocks and arrays of rows:
//   - numbers are JSON integers that fit an int64 (no fraction, no exponent, no leading zero, no "-0");
//   - an object with a duplicate key is refused, naming the key;
//   - strings are printable ASCII with the JSON escapes \" \\ \/ \b \f \n \r \t (no \u: no identifier here needs it);
//   - anything after the top-level value except whitespace is refused.
// Errors carry the byte offset. Pure: no exception crosses the boundary.

namespace cluster_tools { namespace app { namespace genesis { namespace json {

struct Value
{
    enum class Kind : std::uint8_t
    {
        NUL,
        BOOL,
        INT,
        STRING,
        ARRAY,
        OBJECT,
    };

    Kind kind = Kind::NUL;
    bool boolean = false;
    std::int64_t integer = 0;
    std::string text;
    std::vector<Value> items;                           // ARRAY
    std::vector<std::pair<std::string, Value>> members; // OBJECT, in document order
    std::size_t offset = 0;                             // byte offset of the value in the document

    const Value *find(const std::string &key) const;
};

const char *kindName(Value::Kind kind) noexcept;

struct ParseResult
{
    Value value;
    std::string error; // empty on success
    bool ok() const { return error.empty(); }
};

ParseResult parse(const std::string &text);

}}}}

#endif
