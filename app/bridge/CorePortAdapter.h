#ifndef CLUSTER_TOOLS_APP_BRIDGE_CORE_PORT_ADAPTER_H
#define CLUSTER_TOOLS_APP_BRIDGE_CORE_PORT_ADAPTER_H

#include <cstddef>
#include <cstdint>

#include "cluster_tools/client/Protocol.h"
#include "cluster_tools/domain/bridge/Ports.h"
#include "cluster_tools/domain/bridge/Types.h"

#include "AppSeams.h"
#include "BridgeSnapshot.h"

// CorePortAdapter - domain::CorePort over the client's tryOffer: the ONLY path a domain frame takes
// to the wire. The registrar's RegisterSession and the client's keepalives use the same publication, so the FIFO the
// report-loss rule relies on holds across every frame the session sends. The mapping (Types.h):
//   OK -> OK; BACK_PRESSURED, NOT_CONNECTED, NOT_READY -> RETRY (counted bridge_offer_retry_total{status});
//   CLOSED, MAX_POSITION_EXCEEDED -> CLOSED (the domain pauses reads until onSessionDown / onSessionUp);
//   TRANSPORT_ERROR -> REFUSED (the domain sets Fatal::OFFER_REFUSED: exit 70, a bridge bug).
// After an OK offer the observer sees the frame bytes (O3 / O4: the codec's trade getter, metrics only).

namespace cluster_tools { namespace app { namespace bridge {

class OfferObserver
{
public:
    virtual ~OfferObserver() = default;
    virtual void onOfferOk(const std::uint8_t *frame, std::size_t length) noexcept = 0;
};

inline domain::bridge::OfferOutcome mapOfferStatus(client::OfferStatus status) noexcept
{
    using client::OfferStatus;
    using domain::bridge::OfferOutcome;
    switch (status)
    {
        case OfferStatus::OK: return OfferOutcome::OK;
        case OfferStatus::BACK_PRESSURED:
        case OfferStatus::NOT_CONNECTED:
        case OfferStatus::NOT_READY: return OfferOutcome::RETRY;
        case OfferStatus::MAX_POSITION_EXCEEDED:
            return app_seams::disabled(app_seams::Rule::MAX_POSITION_IS_RETRY) ? OfferOutcome::RETRY : OfferOutcome::CLOSED;
        case OfferStatus::CLOSED: return OfferOutcome::CLOSED;
        case OfferStatus::TRANSPORT_ERROR: return OfferOutcome::REFUSED;
    }
    return OfferOutcome::REFUSED;
}

template<typename CoreClient>
class CorePortAdapter final : public domain::bridge::CorePort
{
public:
    CorePortAdapter(CoreClient &client, AppCounters &counters, OfferObserver *observer) :
        m_client(client), m_counters(counters), m_observer(observer)
    {
    }

    domain::bridge::OfferOutcome tryOffer(const std::uint8_t *frame, std::size_t length) noexcept override
    {
        client::OfferResult result{ client::OfferStatus::TRANSPORT_ERROR, 0 };
        try
        {
            result = m_client.tryOffer(frame, length);
        }
        catch (...)
        {
            result = client::OfferResult{ client::OfferStatus::TRANSPORT_ERROR, 0 }; // a transport that throws refuses
        }
        const domain::bridge::OfferOutcome outcome = mapOfferStatus(result.status);
        const std::size_t status = static_cast<std::size_t>(result.status);
        if (domain::bridge::OfferOutcome::RETRY == outcome && status < m_counters.offerRetry.size())
        {
            m_counters.offerRetry[status]++;
        }
        if (domain::bridge::OfferOutcome::OK == outcome && nullptr != m_observer)
        {
            m_observer->onOfferOk(frame, length);
        }
        m_offers++;
        return outcome;
    }

    std::uint64_t offers() const noexcept { return m_offers; }

private:
    CoreClient &m_client;
    AppCounters &m_counters;
    OfferObserver *m_observer;
    std::uint64_t m_offers = 0;
};

}}}

#endif
