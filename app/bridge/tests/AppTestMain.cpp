// doctest main for ct_bridge_app_tests (RC2 S9c 12.2): the signals are blocked before any thread, as main.cpp does
// (StopPathTest drives the real Supervisor).
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"

#include "common/Supervisor.h"

int main(int argc, char **argv)
{
    cluster_tools::app::common::Supervisor::blockSignals();
    doctest::Context context(argc, argv);
    return context.run();
}
