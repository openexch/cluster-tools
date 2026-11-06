#ifndef CLUSTER_TOOLS_CLIENT_TRANSPORT_H
#define CLUSTER_TOOLS_CLIENT_TRANSPORT_H

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string>

// The compile-time port between the cluster client state machine and the media. The real
// implementation is AeronTransport (client/aeron, cpp_wrapper over the Aeron C client); unit
// tests use a scripted fake. Rules every implementation keeps:
//   - nothing blocks: add* only STARTS a registration, *Status polls it;
//   - nothing throws: failures are values (Registration::FAILED, OFFER_TRANSPORT_ERROR);
//   - single-threaded: called only from the owner's duty thread.
// A concept, not a vtable (ports/ClusterSession.h explains why hot paths are compile-time).

namespace cluster_tools { namespace client {

using PublicationId = std::int32_t;
inline constexpr PublicationId NO_PUBLICATION = -1;

enum class Registration : std::uint8_t
{
    PENDING, // the media driver has not answered yet
    READY,
    FAILED,  // the driver refused (bad channel, resource) or the id is unknown/closed
};

// One reassembled egress message (Aeron fragments already joined) and the egress image it
// arrived on, identified by the image's session id (AeronCluster binds the session to it).
struct EgressFragment
{
    const std::uint8_t *data = nullptr;
    std::size_t length = 0;
    std::int32_t imageSessionId = 0;
};

namespace detail {
struct FragmentProbe
{
    void operator()(const EgressFragment &) const {}
};
}

template<typename T>
concept ClusterTransport = requires(
    T &t,
    const std::string &channel,
    std::int32_t streamId,
    PublicationId publication,
    const std::uint8_t *bytes,
    std::size_t length,
    detail::FragmentProbe &probe,
    int fragmentLimit,
    std::int32_t imageSessionId)
{
    { t.nanoTime() } -> std::same_as<std::int64_t>;
    { t.nextCorrelationId() } -> std::same_as<std::int64_t>;

    // Egress: one subscription per transport.
    { t.addEgressSubscription(channel, streamId) } -> std::same_as<bool>;
    { t.egressSubscriptionStatus() } -> std::same_as<Registration>;
    { t.resolvedEgressChannel() } -> std::convertible_to<std::string>; // "" until the port is known
    { t.closeEgressSubscription() } -> std::same_as<void>;
    { t.pollEgress(probe, fragmentLimit) } -> std::same_as<int>;
    { t.resetEgressAssembly() } -> std::same_as<void>;
    { t.isEgressImageClosed(imageSessionId) } -> std::same_as<bool>; // gone or closed

    // Ingress: any number of publications, addressed by transport-local ids.
    { t.addIngressPublication(channel, streamId) } -> std::same_as<PublicationId>;
    { t.publicationStatus(publication) } -> std::same_as<Registration>;
    { t.isPublicationConnected(publication) } -> std::same_as<bool>;
    // Gathers header + body into one message; returns a position (> 0) or an OFFER_* code.
    { t.offer(publication, bytes, length, bytes, length) } -> std::same_as<std::int64_t>;
    { t.closePublication(publication) } -> std::same_as<void>;

    { t.lastError() } -> std::convertible_to<std::string>;
};

}}

#endif
