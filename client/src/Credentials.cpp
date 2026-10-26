#include "cluster_tools/client/Credentials.h"

#include <cctype>
#include <charconv>
#include <cstdlib>

namespace cluster_tools { namespace client {

namespace
{

std::string_view trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
    {
        text.remove_suffix(1);
    }
    return text;
}

bool equalsIgnoreCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); i++)
    {
        if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i])))
        {
            return false;
        }
    }
    return true;
}

int hexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

constexpr SessionClass ALL_CLASSES[] = {
    SessionClass::GATEWAY, SessionClass::FORWARDER, SessionClass::BRIDGE,
    SessionClass::ADMIN, SessionClass::TOOL, SessionClass::TREASURY };

}

const char *nameOf(SessionClass sessionClass) noexcept
{
    switch (sessionClass)
    {
        case SessionClass::UNSET: return "UNSET";
        case SessionClass::GATEWAY: return "GATEWAY";
        case SessionClass::FORWARDER: return "FORWARDER";
        case SessionClass::BRIDGE: return "BRIDGE";
        case SessionClass::ADMIN: return "ADMIN";
        case SessionClass::TOOL: return "TOOL";
        case SessionClass::TREASURY: return "TREASURY";
    }
    return "?";
}

std::vector<std::uint8_t> CoreCredentials::encode() const
{
    std::vector<std::uint8_t> bytes;
    bytes.reserve(ENCODED_LENGTH);
    bytes.push_back(VERSION);
    bytes.push_back(static_cast<std::uint8_t>(sessionClass));
    const auto key = static_cast<std::uint32_t>(keyId);
    for (int shift = 0; shift < 32; shift += 8)
    {
        bytes.push_back(static_cast<std::uint8_t>((key >> shift) & 0xFFu));
    }
    bytes.insert(bytes.end(), secret.begin(), secret.end());
    return bytes;
}

CredentialsResult parseCoreCredentials(std::string_view spec)
{
    CredentialsResult result;
    spec = trim(spec);
    const std::size_t first = spec.find(':');
    const std::size_t second = std::string_view::npos == first ? first : spec.find(':', first + 1);
    if (std::string_view::npos == first || std::string_view::npos == second ||
        std::string_view::npos != spec.find(':', second + 1))
    {
        result.error = "credential spec must be keyId:class:secretHex";
        return result;
    }
    const std::string_view keyText = trim(spec.substr(0, first));
    const std::string_view classText = trim(spec.substr(first + 1, second - first - 1));
    const std::string_view secretText = trim(spec.substr(second + 1));

    std::int32_t keyId = 0;
    const auto parsedKey = std::from_chars(keyText.data(), keyText.data() + keyText.size(), keyId);
    if (keyText.empty() || parsedKey.ec != std::errc() || parsedKey.ptr != keyText.data() + keyText.size())
    {
        result.error = "credential keyId '" + std::string(keyText) + "' is not an int32";
        return result;
    }

    SessionClass sessionClass = SessionClass::UNSET;
    if (!classText.empty() && std::isdigit(static_cast<unsigned char>(classText.front())))
    {
        int code = -1;
        const auto parsedCode = std::from_chars(classText.data(), classText.data() + classText.size(), code);
        if (parsedCode.ec == std::errc() && parsedCode.ptr == classText.data() + classText.size())
        {
            for (SessionClass c : ALL_CLASSES)
            {
                if (static_cast<int>(c) == code)
                {
                    sessionClass = c;
                }
            }
        }
    }
    else
    {
        for (SessionClass c : ALL_CLASSES)
        {
            if (equalsIgnoreCase(classText, nameOf(c)))
            {
                sessionClass = c;
            }
        }
    }
    if (SessionClass::UNSET == sessionClass)
    {
        result.error = "unknown session class '" + std::string(classText) + "'";
        return result;
    }

    if (secretText.size() != 2 * CoreCredentials::SECRET_LENGTH)
    {
        result.error = "secret must be exactly 32 bytes (64 hex chars), got " +
            std::to_string(secretText.size()) + " chars";
        return result;
    }
    for (std::size_t i = 0; i < CoreCredentials::SECRET_LENGTH; i++)
    {
        const int hi = hexValue(secretText[2 * i]);
        const int lo = hexValue(secretText[2 * i + 1]);
        if (hi < 0 || lo < 0)
        {
            result.error = "secret is not hex";
            return result;
        }
        result.credentials.secret[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    result.credentials.sessionClass = sessionClass;
    result.credentials.keyId = keyId;
    return result;
}

CredentialsResult credentialsFromEnvironment(SessionClass requiredClass, const char *variable)
{
    const char *raw = std::getenv(variable);
    if (nullptr == raw || trim(raw).empty())
    {
        CredentialsResult result;
        result.error = std::string(variable) + " is not set (keyId:class:secretHex); refusing to start without a credential";
        return result;
    }
    CredentialsResult result = parseCoreCredentials(raw);
    if (!result.ok())
    {
        result.error = std::string(variable) + ": " + result.error;
        return result;
    }
    if (SessionClass::UNSET != requiredClass && result.credentials.sessionClass != requiredClass)
    {
        result.error = std::string(variable) + " is a " + nameOf(result.credentials.sessionClass) +
            " credential; this process needs " + nameOf(requiredClass);
    }
    return result;
}

}}
