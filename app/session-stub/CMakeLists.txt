# app/session-stub (CT_SESSION_STUB, requires WITH_AERON and CT_BUILD_MEDIA_DRIVER) - ct_session_stub, a TEST-ONLY
# program on the exported client: one GATEWAY core session, a channel mask from the command line, a script from stdin,
# a poll throttle that leaves doWork() and the keepalives running. Never installed (no install rule; cmake/Package.cmake
# installs libraries only) and never published (publish.yml refuses it in the publish tree).
#
#   ct_session_stub               the program; it links the client and the runtime through their cluster_tools:: names,
#                                 as another repository does
#   ct_session_stub_tests         StubOptions, StubCommands and SessionStub over the FakeTransport (session_stub.unit)
#   ct_session_stub_seamed_tests  the same, with the negative-control seams (session_stub.negative_controls)
#   session_stub.process          the binary: 78 on configuration, 70 without a driver, an embedded run driven by stdin

add_library(ct_session_stub_pure STATIC StubOptions.cpp StubCommands.cpp)
target_include_directories(ct_session_stub_pure PUBLIC "${PROJECT_SOURCE_DIR}/app")
target_link_libraries(ct_session_stub_pure PUBLIC cluster_tools::client cluster_tools::runtime)
target_compile_options(ct_session_stub_pure PRIVATE -Wall -Wextra)

add_executable(ct_session_stub main.cpp ${CT_EMBEDDED_DRIVER_SOURCE})
# the embedded driver first (runtime/CMakeLists.txt): its archive resolves the C client symbols before aeron_static
target_link_libraries(ct_session_stub PRIVATE aeron_driver_static ct_session_stub_pure cluster_tools::runtime_aeron
    cluster_tools::client_aeron)
target_compile_options(ct_session_stub PRIVATE -Wall -Wextra)

if(BUILD_TESTING)
    foreach(flavour IN ITEMS plain seamed)
        if(flavour STREQUAL "plain")
            set(name ct_session_stub_tests)
        else()
            set(name ct_session_stub_seamed_tests)
        endif()
        add_executable(${name} tests/SessionStubTest.cpp)
        target_link_libraries(${name} PRIVATE ct_session_stub_pure)
        target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/third_party" "${PROJECT_SOURCE_DIR}/client/tests")
        target_compile_options(${name} PRIVATE -Wall -Wextra)
        if(flavour STREQUAL "seamed")
            target_compile_definitions(${name} PRIVATE CT_SESSION_STUB_SEAMED=1)
        endif()
    endforeach()
    add_test(NAME session_stub.unit COMMAND ct_session_stub_tests)
    add_test(NAME session_stub.negative_controls
        COMMAND bash "${PROJECT_SOURCE_DIR}/archive/tests/run-negative-controls.sh" "$<TARGET_FILE:ct_session_stub_seamed_tests>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/negative-controls.txt")
    add_test(NAME session_stub.process
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/stub-process.sh" "$<TARGET_FILE:ct_session_stub>"
            "${CMAKE_CURRENT_BINARY_DIR}/stub-process")
    set_tests_properties(session_stub.process PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
endif()
