# client - the shared C++ cluster client (RC2 S11): forwarder and bridge both build on it.
#
# ct_client_core   pure parts (endpoints, credentials, backoff, settings, vocabulary): no
#                  Aeron, no SBE; builds and tests in the default WITH_AERON=OFF build.
# client/aeron     (WITH_AERON) the ClusterClient state machine (header-only template over a
#                  ClusterTransport concept, needs the generated cluster codecs), the Aeron
#                  transport, fatal-error handling, the core session registrar (money codecs),
#                  their fake-transport unit tests and the real-cluster integration tests.

add_library(ct_client_core STATIC
    src/Protocol.cpp
    src/IngressEndpoints.cpp
    src/Credentials.cpp
    src/ClientSettings.cpp)
add_library(cluster_tools::client_core ALIAS ct_client_core)
target_include_directories(ct_client_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_options(ct_client_core PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
    add_executable(ct_client_pure_tests tests/ClientPureTest.cpp)
    target_link_libraries(ct_client_pure_tests PRIVATE ct_client_core)
    target_include_directories(ct_client_pure_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
    target_compile_options(ct_client_pure_tests PRIVATE -Wall -Wextra)
    add_test(NAME client.pure COMMAND ct_client_pure_tests)
endif()
