// RC2 S9c 12.2 (main rows that need the real agent): the real Supervisor (bridge settings) around runBridgeDuty over the
// real BridgeAgent, its real invoker and RigSources whose archive calls block:
//   invokerBeatsDuringABlockedArchiveCall   a 6 s blocked call whose invoker beats: no watchdog exit (RED INVOKER_NO_BEAT)
//   sigtermReturns0WhileTheArchiveIsDead    SIGTERM with the current source ARCHIVE_GONE: 0 within 2 s, no stopReplay;
//                                           on a live slow archive (stopReplay blocks 5 s): 0 within 7 s (FIX-7)
//   anEscapedExceptionIs70                  an exception out of the duty iteration: 70 through the catch-all, never 78
//   sigtermDuringABlockedCallOnASlowArchiveReturns0  review FIX-3: SIGTERM lands in a 4.5 s blocked control call and
//                                           stopReplay would block 4.5 s more: 0 within the 7 s join bound, the source
//                                           detached without stopReplay (RED STOP_DETACH_WHEN_LATE: the join bound's 70)
#include "doctest/doctest.h"

#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <thread>

#include "AppRig.h"
#include "BridgeDuty.h"

using namespace ct_bridge_app_test;

namespace {

struct Supervised
{
    AppRig rig;
    app::BridgeBoard board;
    std::atomic<int> exitCode{ -1 };
    std::unique_ptr<common::Supervisor> supervisor;

    explicit Supervised(std::function<void(app::AgentSettings &)> tweak = {}) : rig(1, std::move(tweak))
    {
        supervisor = std::make_unique<common::Supervisor>(board, rig.log, app::bridgeSupervisorSettings(),
            [this](int code, const std::string &) { exitCode.store(code); }, nullptr);
        rig.journal(0, 1, 5);
    }

    int run()
    {
        rig.agent->start();
        return supervisor->run([this] { return app::runBridgeDuty(*rig.agent, *supervisor, board, {}); }, nullptr);
    }
};

void blockFor(std::chrono::milliseconds d, AppRig &r)
{
    const auto end = std::chrono::steady_clock::now() + d;
    while (std::chrono::steady_clock::now() < end)
    {
        r.agent->invoke(); // the archive client calls the invoker after every yield
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

double since(std::chrono::steady_clock::time_point t)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

}

TEST_CASE("StopPathTest.invokerBeatsDuringABlockedArchiveCall")
{
    // the heartbeat is the board, as main.cpp wires it
    Supervised s([](app::AgentSettings &) {});
    app::AgentSettings settings;
    settings.identity = 0xB1;
    settings.sourceCount = 1;
    s.rig.agent = std::make_unique<AppRig::Agent>(*s.rig.client, s.rig.sources, s.rig.tap, *s.rig.memo, s.rig.archiveMetrics,
        s.rig.encoder, settings, &s.rig.log, &s.board);
    s.rig.observer.target = s.rig.agent.get();
    s.rig.sources.duringDoWork = [&] { blockFor(std::chrono::milliseconds(6000), s.rig); };
    std::thread killer([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(6600));
        ::kill(::getpid(), SIGTERM);
    });
    const int code = s.run();
    killer.join();
    const bool survivedTheBlockedCall = 0 == code && -1 == s.exitCode.load();
    CHECK(survivedTheBlockedCall); // INVOKER_NO_BEAT RED: the watchdog fires at 5 s
    CHECK(s.rig.agent->invokes() > 1000);
}

TEST_CASE("StopPathTest.sigtermReturns0WhileTheArchiveIsDead")
{
    for (const bool dead : { true, false })
    {
        CAPTURE(dead);
        Supervised s;
        s.rig.sources.duringStop = [&] { blockFor(std::chrono::milliseconds(5000), s.rig); }; // stopReplay blocks 5 s
        // the hook re-arms itself with a copy of `hook` on every duty iteration until the stop: it must live as long as
        // the run, not only as long as the if block (a copy of a destroyed std::function is undefined behaviour; on
        // aarch64 the re-armed copy came out empty, the fault was never injected and the stop ran stopReplay)
        std::function<void()> hook;
        if (dead)
        {
            // the archive dies: its last fault is ARCHIVE_GONE when the stop arrives (on the duty thread)
            hook = [&] {
                if (s.supervisor->stopRequested.load())
                {
                    SourceFault gone;
                    gone.source = 0;
                    gone.kind = FaultKind::ARCHIVE_GONE;
                    s.rig.tap.onFault(gone);
                    return;
                }
                s.rig.sources.duringDoWork = hook;
            };
            s.rig.sources.duringDoWork = hook;
        }
        std::atomic<std::int64_t> signalled{ 0 };
        const auto t0 = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point signalledAt;
        std::thread killer([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            signalledAt = std::chrono::steady_clock::now();
            ::kill(::getpid(), SIGTERM);
        });
        const int code = s.run();
        killer.join();
        const double took = since(signalledAt);
        (void)t0;
        MESSAGE(std::string(dead ? "dead archive: " : "live slow archive: ") << took << " s from SIGTERM to the return");
        CHECK(code == 0);
        CHECK(s.exitCode.load() == -1);
        CHECK(s.rig.agent->stopReason().exitCode == 0);
        if (dead)
        {
            CHECK(s.rig.sources.stops == 0); // detached without stopReplay
            CHECK(took < 2.0);
        }
        else
        {
            CHECK(s.rig.sources.stops == 1);
            CHECK(took < 7.0);
            CHECK(took > 4.5);
        }
    }
}

TEST_CASE("StopPathTest.sigtermDuringABlockedCallOnASlowArchiveReturns0")
{
    Supervised s;
    s.rig.sources.duringStop = [&] { blockFor(std::chrono::milliseconds(4500), s.rig); }; // a slow, live archive
    const auto t0 = std::chrono::steady_clock::now();
    std::atomic<bool> blocked{ false };
    std::function<void()> hook; // the rig's hook is one-shot: re-armed until it has blocked once
    hook = [&] {
        if (since(t0) > 0.2)
        {
            blocked.store(true);
            blockFor(std::chrono::milliseconds(4500), s.rig); // a startReplay / listRecordings in a 4.5 s GC pause
            return;
        }
        s.rig.sources.duringDoWork = hook;
    };
    s.rig.sources.duringDoWork = hook;
    std::chrono::steady_clock::time_point signalledAt;
    std::thread killer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(500)); // inside the blocked call
        signalledAt = std::chrono::steady_clock::now();
        ::kill(::getpid(), SIGTERM);
    });
    const int code = s.run();
    killer.join();
    const double took = since(signalledAt);
    // the RED shape returns 70 at the join bound with the duty thread still inside stopReplay (detached): let it finish
    // before the rig goes away
    for (int i = 0; i < 100 && !s.rig.agent->stopped(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    MESSAGE("SIGTERM in a blocked call, slow stopReplay: exit " << code << " after " << took << " s, stopReplay calls "
                                                                 << s.rig.sources.stops);
    const bool returned0 = 0 == code && -1 == s.exitCode.load() && 0 == s.rig.agent->stopReason().exitCode;
    CHECK(returned0); // STOP_DETACH_WHEN_LATE RED: 4.2 s left in the call + 4.5 s stopReplay > 7 s: the join bound's 70
    CHECK(blocked.load());
    CHECK(s.rig.sources.stops == 0); // detached without stopReplay
    CHECK(took < 7.0);
}

TEST_CASE("StopPathTest.anEscapedExceptionIs70")
{
    Supervised s;
    s.rig.sources.duringDoWork = [] { throw std::runtime_error("a codec bug (test double)"); };
    const int code = s.run();
    CHECK(code == 70);
    CHECK(s.exitCode.load() == 70);
    CHECK(s.supervisor->dutyFatal.load() == 70);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
