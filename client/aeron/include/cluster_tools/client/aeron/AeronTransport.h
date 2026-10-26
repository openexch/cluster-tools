#ifndef CLUSTER_TOOLS_CLIENT_AERON_AERON_TRANSPORT_H
#define CLUSTER_TOOLS_CLIENT_AERON_AERON_TRANSPORT_H

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include <Aeron.h>
#include <FragmentAssembler.h>

#include "cluster_tools/client/Protocol.h"
#include "cluster_tools/client/Transport.h"
#include "cluster_tools/client/aeron/FatalErrors.h"

// AeronTransport - the ClusterTransport over the official Aeron cpp_wrapper (C client).
// One instance per cluster session; several instances may share ONE aeron::Aeron (one client
// conductor for the whole process: the core session and the ME session of the forwarder).
//
// Non-blocking throughout: registrations use the wrapper's async add + find, and nothing
// here waits for the driver. Exceptions from the wrapper never escape: a failed registration
// is Registration::FAILED, a failed offer is OFFER_TRANSPORT_ERROR, both with lastError().
// Fatal client errors (driver timeout, client timeout) do not surface here; they reach the
// AeronFatalMonitor installed in the aeron::Context (FatalErrors.h).
//
// The egress hot path allocates nothing: the FragmentAssembler is created once per session
// (and on a new leader), the poll trampoline is a function pointer + context, and the bound
// egress image is looked up once and cached.

namespace cluster_tools { namespace client { namespace aeron_transport {

static_assert(::aeron::NOT_CONNECTED == OFFER_NOT_CONNECTED);
static_assert(::aeron::BACK_PRESSURED == OFFER_BACK_PRESSURED);
static_assert(::aeron::ADMIN_ACTION == OFFER_ADMIN_ACTION);
static_assert(::aeron::PUBLICATION_CLOSED == OFFER_PUBLICATION_CLOSED);
static_assert(::aeron::MAX_POSITION_EXCEEDED == OFFER_MAX_POSITION_EXCEEDED);

class AeronTransport
{
public:
    explicit AeronTransport(std::shared_ptr<::aeron::Aeron> aeron) : m_aeron(std::move(aeron))
    {
        // The handler the subscription polls with never changes; it forwards every fragment to
        // whichever assembler is active, so the assembler can be replaced in the middle of a
        // poll (resetEgressAssembly at a NewLeaderEvent) without touching the running callback.
        m_pollHandler = [this](::aeron::concurrent::AtomicBuffer &buffer, ::aeron::util::index_t offset,
            ::aeron::util::index_t length, ::aeron::concurrent::logbuffer::Header &header)
        {
            m_active->handler(buffer, offset, length, header);
        };
        makeAssembler();
    }

    ~AeronTransport()
    {
        closeEgressSubscription();
        for (std::size_t i = 0; i < m_publications.size(); i++)
        {
            closePublication(static_cast<PublicationId>(i));
        }
    }

    AeronTransport(const AeronTransport &) = delete;
    AeronTransport &operator=(const AeronTransport &) = delete;

    std::int64_t nanoTime() const
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::int64_t nextCorrelationId()
    {
        try
        {
            return m_aeron->nextCorrelationId();
        }
        catch (const std::exception &e)
        {
            m_error = e.what();
            return ++m_fallbackCorrelation; // the client is dying anyway; the fatal monitor says so
        }
    }

    // ---------------------------------------------------------------- egress

    bool addEgressSubscription(const std::string &channel, std::int32_t streamId)
    {
        closeEgressSubscription();
        m_subscriptionChannel = channel;
        m_subscriptionStreamId = streamId;
        try
        {
            m_subscriptionRegistration = m_aeron->addSubscription(channel, streamId);
            m_subscriptionStatus = Registration::PENDING;
            return true;
        }
        catch (const std::exception &e)
        {
            m_error = std::string("addSubscription(") + channel + "): " + e.what();
            m_subscriptionStatus = Registration::FAILED;
            return false;
        }
    }

    Registration egressSubscriptionStatus()
    {
        if (Registration::PENDING == m_subscriptionStatus)
        {
            try
            {
                m_subscription = m_aeron->findSubscription(m_subscriptionRegistration);
                if (m_subscription)
                {
                    m_subscriptionStatus = Registration::READY;
                    m_subscriptionRegistration = -1;
                }
            }
            catch (const std::exception &e)
            {
                m_error = std::string("egress subscription: ") + e.what();
                m_subscriptionRegistration = -1;
                m_subscriptionStatus = Registration::FAILED;
                if (isTransientRegistrationError(e))
                {
                    // Re-issue; the client's connect deadline bounds the retries.
                    try
                    {
                        m_subscriptionRegistration = m_aeron->addSubscription(m_subscriptionChannel, m_subscriptionStreamId);
                        m_subscriptionStatus = Registration::PENDING;
                        m_transientRetries++;
                    }
                    catch (const std::exception &again)
                    {
                        m_error = std::string("egress subscription retry: ") + again.what();
                    }
                }
            }
        }
        return m_subscriptionStatus;
    }

    std::string resolvedEgressChannel() const
    {
        if (!m_subscription)
        {
            return {};
        }
        try
        {
            return m_subscription->tryResolveChannelEndpointPort();
        }
        catch (const std::exception &)
        {
            return {};
        }
    }

    void closeEgressSubscription()
    {
        if (m_subscriptionRegistration >= 0)
        {
            m_abandonedSubscriptions.push_back(m_subscriptionRegistration);
            m_subscriptionRegistration = -1;
        }
        m_boundImage.reset();
        m_boundImageSessionId = 0;
        m_subscription.reset();
        m_subscriptionStatus = Registration::FAILED;
        makeAssembler();
    }

    template<typename Handler>
    int pollEgress(Handler &&handler, int fragmentLimit)
    {
        if (!m_subscription)
        {
            return 0;
        }
        using H = std::remove_reference_t<Handler>;
        m_sinkContext = const_cast<void *>(static_cast<const void *>(&handler));
        m_sink = [](void *context, const EgressFragment &fragment) { (*static_cast<H *>(context))(fragment); };
        struct Reset
        {
            AeronTransport &t;
            ~Reset()
            {
                t.m_sink = nullptr;
                t.m_sinkContext = nullptr;
                t.m_inPoll = false;
                t.m_retired.clear(); // assemblers replaced during this poll: no callback runs in them now
            }
        } reset{ *this };
        m_inPoll = true;
        return m_subscription->poll(m_pollHandler, fragmentLimit);
    }

    // Drops every partial reassembly, effective for the NEXT fragment, also when called from
    // inside a poll callback (AeronCluster clears its assembler inside onNewLeader). The
    // assembler whose callback is running is retired, not destroyed, until the poll returns.
    void resetEgressAssembly()
    {
        makeAssembler();
    }

    bool isEgressImageClosed(std::int32_t imageSessionId)
    {
        if (!m_subscription)
        {
            return true;
        }
        if (!m_boundImage || m_boundImageSessionId != imageSessionId)
        {
            m_boundImage = m_subscription->imageBySessionId(imageSessionId);
            m_boundImageSessionId = imageSessionId;
            if (!m_boundImage)
            {
                return true;
            }
        }
        return m_boundImage->isClosed();
    }

    // ---------------------------------------------------------------- ingress

    PublicationId addIngressPublication(const std::string &channel, std::int32_t streamId)
    {
        reapAbandoned();
        Slot slot;
        slot.channel = channel;
        slot.streamId = streamId;
        try
        {
            slot.registration = m_aeron->addPublication(channel, streamId);
        }
        catch (const std::exception &e)
        {
            m_error = std::string("addPublication(") + channel + "): " + e.what();
            return NO_PUBLICATION;
        }
        for (std::size_t i = 0; i < m_publications.size(); i++)
        {
            if (m_publications[i].free)
            {
                m_publications[i] = std::move(slot);
                return static_cast<PublicationId>(i);
            }
        }
        m_publications.push_back(std::move(slot));
        return static_cast<PublicationId>(m_publications.size() - 1);
    }

    Registration publicationStatus(PublicationId id)
    {
        Slot *slot = find(id);
        if (nullptr == slot)
        {
            return Registration::FAILED;
        }
        if (Registration::PENDING == slot->status)
        {
            try
            {
                slot->publication = m_aeron->findPublication(slot->registration);
                if (slot->publication)
                {
                    slot->status = Registration::READY;
                    slot->registration = -1;
                }
            }
            catch (const std::exception &e)
            {
                m_error = std::string("ingress publication: ") + e.what();
                slot->registration = -1;
                slot->status = Registration::FAILED;
                if (isTransientRegistrationError(e))
                {
                    // RESOURCE_TEMPORARILY_UNAVAILABLE: add the same publication again and stay
                    // PENDING; the client's messageTimeout deadline bounds the retries.
                    try
                    {
                        slot->registration = m_aeron->addPublication(slot->channel, slot->streamId);
                        slot->status = Registration::PENDING;
                        m_transientRetries++;
                    }
                    catch (const std::exception &again)
                    {
                        m_error = std::string("ingress publication retry: ") + again.what();
                    }
                }
            }
        }
        return slot->status;
    }

    bool isPublicationConnected(PublicationId id)
    {
        Slot *slot = find(id);
        return nullptr != slot && slot->publication && slot->publication->isConnected();
    }

    std::int64_t offer(PublicationId id, const std::uint8_t *header, std::size_t headerLength,
        const std::uint8_t *body, std::size_t bodyLength)
    {
        Slot *slot = find(id);
        if (nullptr == slot || !slot->publication)
        {
            return OFFER_NOT_CONNECTED;
        }
        try
        {
            if (0 == bodyLength)
            {
                ::aeron::concurrent::AtomicBuffer buffer(const_cast<std::uint8_t *>(header), headerLength);
                return slot->publication->offer(buffer, 0, static_cast<::aeron::util::index_t>(headerLength));
            }
            std::array<::aeron::concurrent::AtomicBuffer, 2> buffers{
                ::aeron::concurrent::AtomicBuffer(const_cast<std::uint8_t *>(header), headerLength),
                ::aeron::concurrent::AtomicBuffer(const_cast<std::uint8_t *>(body), bodyLength) };
            return slot->publication->offer(buffers);
        }
        catch (const std::exception &e)
        {
            m_error = std::string("offer: ") + e.what();
            return OFFER_TRANSPORT_ERROR;
        }
    }

    void closePublication(PublicationId id)
    {
        Slot *slot = find(id);
        if (nullptr == slot)
        {
            return;
        }
        if (slot->registration >= 0)
        {
            m_abandonedPublications.push_back(slot->registration);
        }
        *slot = Slot{};
        slot->free = true;
    }

    std::string lastError() const
    {
        return m_error;
    }

    ::aeron::Aeron &aeron() { return *m_aeron; }
    std::uint64_t transientRetries() const { return m_transientRetries; }

private:
    struct Slot
    {
        std::int64_t registration = -1;
        std::shared_ptr<::aeron::Publication> publication;
        Registration status = Registration::PENDING;
        std::string channel;
        std::int32_t streamId = 0;
        bool free = false;
    };

    Slot *find(PublicationId id)
    {
        if (id < 0 || static_cast<std::size_t>(id) >= m_publications.size() ||
            m_publications[static_cast<std::size_t>(id)].free)
        {
            return nullptr;
        }
        return &m_publications[static_cast<std::size_t>(id)];
    }

    // An async add that completes after its owner gave up must still be released, or the
    // publication would live (and keep a driver resource) until the Aeron client closes.
    void reapAbandoned()
    {
        for (auto it = m_abandonedPublications.begin(); it != m_abandonedPublications.end();)
        {
            try
            {
                if (nullptr == m_aeron->findPublication(*it))
                {
                    ++it; // still pending at the driver
                    continue;
                }
                it = m_abandonedPublications.erase(it); // found, and released right here
            }
            catch (const std::exception &)
            {
                it = m_abandonedPublications.erase(it); // the add failed: nothing to release
            }
        }
        for (auto it = m_abandonedSubscriptions.begin(); it != m_abandonedSubscriptions.end();)
        {
            try
            {
                if (m_aeron->findSubscription(*it) == nullptr)
                {
                    ++it;
                    continue;
                }
                it = m_abandonedSubscriptions.erase(it);
            }
            catch (const std::exception &)
            {
                it = m_abandonedSubscriptions.erase(it);
            }
        }
    }

    struct ActiveAssembler
    {
        std::unique_ptr<::aeron::FragmentAssembler> assembler;
        ::aeron::fragment_handler_t handler;
    };

    void makeAssembler()
    {
        if (m_active && m_inPoll)
        {
            m_retired.push_back(std::move(m_active)); // the object itself does not move
        }
        auto next = std::make_unique<ActiveAssembler>();
        next->assembler = std::make_unique<::aeron::FragmentAssembler>(
            [this](::aeron::concurrent::AtomicBuffer &buffer, ::aeron::util::index_t offset,
                ::aeron::util::index_t length, ::aeron::concurrent::logbuffer::Header &header)
            {
                if (nullptr != m_sink)
                {
                    m_sink(m_sinkContext, EgressFragment{
                        buffer.buffer() + offset, static_cast<std::size_t>(length), header.sessionId() });
                }
            });
        next->handler = next->assembler->handler();
        m_active = std::move(next);
    }

    std::shared_ptr<::aeron::Aeron> m_aeron;
    std::string m_error;
    std::int64_t m_fallbackCorrelation = 1LL << 62;

    std::uint64_t m_transientRetries = 0;
    std::string m_subscriptionChannel;
    std::int32_t m_subscriptionStreamId = 0;
    std::int64_t m_subscriptionRegistration = -1;
    Registration m_subscriptionStatus = Registration::FAILED;
    std::shared_ptr<::aeron::Subscription> m_subscription;
    std::shared_ptr<::aeron::Image> m_boundImage;
    std::int32_t m_boundImageSessionId = 0;

    std::unique_ptr<ActiveAssembler> m_active;
    std::vector<std::unique_ptr<ActiveAssembler>> m_retired;
    ::aeron::fragment_handler_t m_pollHandler;
    bool m_inPoll = false;
    void (*m_sink)(void *, const EgressFragment &) = nullptr;
    void *m_sinkContext = nullptr;

    std::vector<Slot> m_publications;
    std::vector<std::int64_t> m_abandonedPublications;
    std::vector<std::int64_t> m_abandonedSubscriptions;
};

static_assert(ClusterTransport<AeronTransport>);

}}}

#endif
