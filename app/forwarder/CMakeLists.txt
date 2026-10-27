# app/forwarder - ct-forwarder, the RC2 S12 production forwarder.
#
# Every configuration (the WITH_AERON=OFF fast job included):
#   ct_forwarder_pure          ForwarderConfig (strict env), MetricsRender, HttpServer, Supervisor (main's loop)
#   ct_forwarder_config_tests  ForwarderConfigTest + MetricsRenderTest
#   ct_forwarder_main_tests    the watchdog, the 2 s join bound and the stop path against duty stubs (FIX-8)
# WITH_AERON:
#   ct-forwarder               the published binary (publish.yml FILES + manifest)
#   ct_forwarder_app_tests     ForwarderAgent over two FakeTransports (contract 11.3)
# CT_FORWARDER_TEST_BUILDS (never published; publish.yml refuses them):
#   ct_forwarder_nc_noresync / _nc_norelease / _nc_refuse  negative-control presets of 11.4
#   ct_forwarder_tc_corrupt                                 T1-G: a core defect simulated after decode
#
# The domain library flavour is chosen by each final target (production or seamed), never by a library, so a
# binary holds exactly one copy of the domain.

add_library(ct_forwarder_pure STATIC
    ForwarderConfig.cpp
    MetricsRender.cpp
    HttpServer.cpp
    Supervisor.cpp)
target_include_directories(ct_forwarder_pure PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/domain/forwarder/include")
target_link_libraries(ct_forwarder_pure PUBLIC ct_client_core Threads::Threads)
target_compile_options(ct_forwarder_pure PRIVATE -Wall -Wextra)

if(BUILD_TESTING)
    add_executable(ct_forwarder_config_tests
        tests/PureTestMain.cpp
        tests/ForwarderConfigTest.cpp
        tests/MetricsRenderTest.cpp)
    target_link_libraries(ct_forwarder_config_tests PRIVATE ct_forwarder_pure ct_forwarder_domain)
    target_include_directories(ct_forwarder_config_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
    target_compile_definitions(ct_forwarder_config_tests PRIVATE
        CT_FORWARDER_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden"
        CT_FORWARDER_ALERTS_FILE="${CMAKE_CURRENT_SOURCE_DIR}/alerts.yml")
    target_compile_options(ct_forwarder_config_tests PRIVATE -Wall -Wextra)
    add_test(NAME forwarder.config COMMAND ct_forwarder_config_tests)

    add_executable(ct_forwarder_main_tests tests/MainTest.cpp)
    target_link_libraries(ct_forwarder_main_tests PRIVATE ct_forwarder_pure ct_forwarder_domain)
    target_include_directories(ct_forwarder_main_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
    target_compile_options(ct_forwarder_main_tests PRIVATE -Wall -Wextra)
    add_test(NAME forwarder.main COMMAND ct_forwarder_main_tests)
    set_tests_properties(forwarder.main PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
endif()

if(WITH_AERON)
    function(ct_forwarder_binary name domain)
        add_executable(${name} main.cpp)
        target_link_libraries(${name} PRIVATE ct_forwarder_pure ct_forwarder_codec ${domain} ct_client_aeron ct_sbe_codecs)
        target_compile_options(${name} PRIVATE -Wall -Wextra)
        if(DEFINED CT_FORWARDER_VERSION_STRING)
            target_compile_definitions(${name} PRIVATE CT_FORWARDER_VERSION="${CT_FORWARDER_VERSION_STRING}")
        endif()
    endfunction()
    ct_forwarder_binary(ct-forwarder ct_forwarder_domain)

    if(BUILD_TESTING)
        add_executable(ct_forwarder_app_tests tests/AppTestMain.cpp tests/ForwarderAgentTest.cpp tests/ZeroAllocTest.cpp)
        target_link_libraries(ct_forwarder_app_tests PRIVATE ct_forwarder_pure ct_forwarder_codec ct_forwarder_domain_seamed
            ct_client ct_sbe_codecs)
        target_include_directories(ct_forwarder_app_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party"
            "${PROJECT_SOURCE_DIR}/client/tests" "${PROJECT_SOURCE_DIR}/adapters/sbe/tests")
        target_compile_options(ct_forwarder_app_tests PRIVATE -Wall -Wextra)
        add_test(NAME forwarder.app COMMAND ct_forwarder_app_tests)

        add_test(NAME forwarder.app_negative_controls
            COMMAND bash "${PROJECT_SOURCE_DIR}/archive/tests/run-negative-controls.sh" "$<TARGET_FILE:ct_forwarder_app_tests>"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/negative-controls.txt")
        set_tests_properties(forwarder.app_negative_controls PROPERTIES TIMEOUT 900)
    endif()

    # T1 (contract 11.4): 3 AE + 3 ME + 2 forwarders on this host, serial. Needs the match jar with the S12 W9 epoch
    # hello (a busy ME otherwise never shows a new session its epoch) and the negative-control binaries.
    set(CT_IT_FORWARDER_MATCH_JAR "" CACHE FILEPATH "match-cluster.jar at the S6 merge + RC2 S12 W9 (the T1 forwarder run)")
    if(CT_INTEGRATION_TESTS AND CT_FORWARDER_TEST_BUILDS AND EXISTS "${CT_IT_FORWARDER_MATCH_JAR}")
        add_test(NAME forwarder.integration
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/it/run-forwarder-it.sh" "${CMAKE_BINARY_DIR}" "${CT_IT_ASSETS_JAR}"
                "${CT_IT_FORWARDER_MATCH_JAR}")
        set_tests_properties(forwarder.integration PROPERTIES TIMEOUT 2400 RUN_SERIAL TRUE)
    endif()

    if(CT_FORWARDER_TEST_BUILDS)
        ct_forwarder_binary(ct_forwarder_nc_noresync ct_forwarder_domain_seamed)
        target_compile_definitions(ct_forwarder_nc_noresync PRIVATE CT_TEST_SEAMS=1 CT_FORWARDER_NC_PRESET="RESYNC_ALL_TRIGGERS")
        ct_forwarder_binary(ct_forwarder_nc_norelease ct_forwarder_domain_seamed)
        target_compile_definitions(ct_forwarder_nc_norelease PRIVATE CT_TEST_SEAMS=1 CT_FORWARDER_NC_PRESET="AGENT_RELEASE_RELAY")
        ct_forwarder_binary(ct_forwarder_nc_refuse ct_forwarder_domain_seamed)
        target_compile_definitions(ct_forwarder_nc_refuse PRIVATE CT_TEST_SEAMS=1 CT_FORWARDER_NC_PRESET="AGENT_SEND_INVALID")
        ct_forwarder_binary(ct_forwarder_tc_corrupt ct_forwarder_domain_seamed)
        target_compile_definitions(ct_forwarder_tc_corrupt PRIVATE CT_TEST_SEAMS=1 CT_FORWARDER_TC_CORRUPT=1)
    endif()
endif()
