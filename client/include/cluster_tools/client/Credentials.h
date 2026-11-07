#ifndef CLUSTER_TOOLS_CLIENT_CREDENTIALS_H
#define CLUSTER_TOOLS_CLIENT_CREDENTIALS_H

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Core credentials K6 v1, byte-compatible with assets
// com.openexchange.assets.auth.CoreCredentials: [u8 version=1][u8 class][i32 keyId LE]
// [32 bytes secret] = 38 bytes; spec string "keyId:class:secretHex" with the class by name
// (case-insensitive) or numeric code; env AE_AUTH_CREDENTIAL. Strict, pure.

namespace cluster_tools { namespace client {

enum class SessionClass : std::uint8_t
{
    UNSET = 0,
    GATEWAY = 1,
    FORWARDER = 2,
    BRIDGE = 3,
    ADMIN = 4,
    TOOL = 5,
    TREASURY = 6,
};

const char *nameOf(SessionClass sessionClass) noexcept;

struct CoreCredentials
{
    static constexpr std::uint8_t VERSION = 1;
    static constexpr std::size_t SECRET_LENGTH = 32;
    static constexpr std::size_t ENCODED_LENGTH = 1 + 1 + 4 + SECRET_LENGTH;
    static constexpr const char *ENV = "AE_AUTH_CREDENTIAL";

    SessionClass sessionClass = SessionClass::UNSET;
    std::int32_t keyId = 0;
    std::array<std::uint8_t, SECRET_LENGTH> secret{};

    std::vector<std::uint8_t> encode() const;
};

struct CredentialsResult
{
    CoreCredentials credentials;
    std::string error; // empty on success
    bool ok() const { return error.empty(); }
};

CredentialsResult parseCoreCredentials(std::string_view spec);

// Reads the spec from the environment variable (default AE_AUTH_CREDENTIAL) and, when
// requiredClass is not UNSET, asserts the class (a FORWARDER binary started with a BRIDGE key
// is a config error, refused at start like AeFeedClient does). Missing or empty = error.
CredentialsResult credentialsFromEnvironment(
    SessionClass requiredClass, const char *variable = CoreCredentials::ENV);

}}

#endif
