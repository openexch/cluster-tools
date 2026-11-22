#ifndef CLUSTER_TOOLS_APP_GENESIS_CLUSTER_LINK_H
#define CLUSTER_TOOLS_APP_GENESIS_CLUSTER_LINK_H

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cluster_tools/client/ClusterClient.h"
#include "genesis/GenesisRunner.h"

// ClusterLink - the genesis runner's Link over the exported cluster client (ClusterClient), blocking in the simplest
// way a one-shot tool can afford: every wait is a loop of doWork() + poll() with a 1 ms pause when nothing happened,
// bounded by the caller's timeout. Egress that arrives while connecting or offering is queued and handed to the next
// pumpUntil(). A session the cluster closes ends every wait with the close reason; a leader change is followed by the
// client (a reply lost with the ended term surfaces as a timeout, and the phase fails: status then shows whether it is
// a prefix). `stale` is the owner's conductor check (exit 70 on a dead driver), run on every loop.

namespace cluster_tools { namespace app { namespace genesis {

template<typename Transport>
class ClusterLink : public Link
{
public:
    using Client = client::ClusterClient<Transport>;

    // The link owns its transport: one egress subscription per session.
    ClusterLink(std::unique_ptr<Transport> transport, client::ClientSettings settings, std::function<void()> stale) :
        m_transport(std::move(transport)),
        m_client(*m_transport, std::move(settings)),
        m_stale(std::move(stale))
    {
    }

    bool open(std::int64_t timeoutMs, std::string &error) override
    {
        if (m_client.isConnected())
        {
            return true;
        }
        if (!live() && !m_client.connect())
        {
            error = "connect refused: " + m_client.lastDetail();
            return false;
        }
        const auto deadline = now() + std::chrono::milliseconds(timeoutMs);
        while (now() < deadline)
        {
            const int work = step();
            if (m_client.isConnected())
            {
                return true;
            }
            if (client::ClientState::FAILED == m_client.state() || client::ClientState::CLOSED == m_client.state())
            {
                error = std::string(client::nameOf(m_client.connectError())) + ": " + m_client.lastDetail();
                return false;
            }
            idle(work);
        }
        error = "not connected within " + std::to_string(timeoutMs) + " ms (state " +
            client::nameOf(m_client.state()) + ": " + m_client.lastDetail() + ")";
        return false;
    }

    bool send(const wire::Bytes &frame, std::int64_t timeoutMs, std::string &error) override
    {
        const auto deadline = now() + std::chrono::milliseconds(timeoutMs);
        client::OfferResult last;
        while (now() < deadline)
        {
            last = m_client.tryOffer(frame.data(), frame.size());
            if (last.ok())
            {
                return true;
            }
            if (!live())
            {
                error = lostText();
                return false;
            }
            if (client::OfferStatus::TRANSPORT_ERROR == last.status ||
                client::OfferStatus::MAX_POSITION_EXCEEDED == last.status)
            {
                error = std::string("offer failed: ") + client::nameOf(last.status);
                return false;
            }
            idle(step());
        }
        error = std::string("not offered within ") + std::to_string(timeoutMs) + " ms (last " +
            client::nameOf(last.status) + ")";
        return false;
    }

    bool pumpUntil(const FrameHandler &onFrame, std::int64_t timeoutMs, std::string &error) override
    {
        const auto deadline = now() + std::chrono::milliseconds(timeoutMs);
        for (;;)
        {
            while (!m_queue.empty())
            {
                const std::vector<std::uint8_t> frame = std::move(m_queue.front());
                m_queue.pop_front();
                if (onFrame(frame.data(), frame.size()))
                {
                    return true;
                }
            }
            if (!live())
            {
                error = lostText();
                return false;
            }
            if (now() >= deadline)
            {
                error.clear(); // a timeout, not a lost session: the caller names the wait
                return false;
            }
            const int work = step();
            if (m_queue.empty())
            {
                idle(work);
            }
        }
    }

    void pause(std::int64_t ms) override
    {
        const auto until = now() + std::chrono::milliseconds(ms);
        while (now() < until)
        {
            idle(step()); // keepalives and egress keep flowing while the caller waits
        }
    }

    void close() override
    {
        m_client.close();
    }

private:
    static std::chrono::steady_clock::time_point now()
    {
        return std::chrono::steady_clock::now();
    }

    bool live() const
    {
        const client::ClientState s = m_client.state();
        return client::ClientState::CONNECTED == s || client::ClientState::CONNECTING == s ||
            client::ClientState::AWAIT_NEW_LEADER_CONNECTION == s || client::ClientState::AWAIT_NEW_LEADER == s;
    }

    std::string lostText() const
    {
        return std::string("session lost (") + client::nameOf(m_client.state()) + ", " +
            client::nameOf(m_client.closeReason()) + "): " + m_client.lastDetail();
    }

    int step()
    {
        if (m_stale)
        {
            m_stale();
        }
        int work = m_client.doWork();
        work += m_client.poll(
            [this](const client::EgressMessage &m) { m_queue.emplace_back(m.payload, m.payload + m.length); }, 16);
        return work;
    }

    static void idle(int work)
    {
        if (0 == work)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    std::unique_ptr<Transport> m_transport;
    Client m_client;
    std::function<void()> m_stale;
    std::deque<std::vector<std::uint8_t>> m_queue;
};

}}}

#endif
