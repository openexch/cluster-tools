#ifndef CLUSTER_TOOLS_PORTS_RESULT_H
#define CLUSTER_TOOLS_PORTS_RESULT_H

#include <string>
#include <utility>

// Result<T> — tiny expected-style carrier for port return values.
//
// The hexagon's contract is that no exception crosses a port boundary: adapters wrap
// third-party code that throws (the aeron cpp_wrapper does) and convert at the seam.
// C++17 has no std::expected and this repo takes no dependencies, so this ~40-line
// class does the job. T must be default-constructible (all port value types are).

namespace cluster_tools { namespace ports {

template <typename T>
class Result
{
public:
    static Result success(T value)
    {
        return Result(true, std::move(value), std::string());
    }

    static Result failure(std::string error)
    {
        return Result(false, T(), std::move(error));
    }

    bool ok() const
    {
        return m_ok;
    }

    // Meaningful only when ok().
    const T &value() const
    {
        return m_value;
    }

    // Meaningful only when !ok(); a human-readable reason, never empty on failure.
    const std::string &error() const
    {
        return m_error;
    }

private:
    Result(bool ok, T value, std::string error) :
        m_ok(ok), m_value(std::move(value)), m_error(std::move(error))
    {
    }

    bool m_ok;
    T m_value;
    std::string m_error;
};

}}

#endif
