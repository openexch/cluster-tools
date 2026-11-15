// doctest main for ct_bridge_app_tests: the signals are blocked before any thread, as main.cpp does
// (StopPathTest drives the real Supervisor).
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"

#include "cluster_tools/runtime/Supervisor.h"

int main(int argc, char **argv)
{
    cluster_tools::runtime::Supervisor::blockSignals();
    doctest::Context context(argc, argv);
    return context.run();
}
