# app/bridge - ct-bridge, the RC2 S9c production C++ settlement bridge.
#
# Every configuration (the WITH_AERON=OFF fast job included):
#   ct_bridge_pure / _seamed   BridgeConfig (strict env), BridgeMetricsRender (/health /ready /metrics, readiness), the
#                              app seams; links ct_app_common, the domain and the archive core
#   ct_bridge_config_tests     BridgeConfigTest + MetricsRenderTest + AckMapTest (12.1)
#   ct_bridge_main_tests       the watchdog, STALLED, the 7 s join bound, SIGHUP counted, against duty stubs (12.2 main)
# WITH_AERON AND WITH_BRIDGE (the archive adapter and the embedded driver link the verified tree):
#   ct-bridge                  the published binary (publish.yml FILES + manifest)
#   ct_bridge_app_tests        BridgeAgent over FakeTransport + SimSources (12.2)
#   ct_bridge_app_alloc_tests  zeroAllocDutyIteration (own binary: it replaces the global operator new)
#   ct_bridge_main_aeron_tests startSequenceBothModes, conductorStallExits70 (a real driver)
#   ct_bridge_e3_app_tests     12.6 E3 through the agent with serve as the core (needs the harness jar)
# CT_BRIDGE_TEST_BUILDS (never published; publish.yml refuses them):
#   ct_bridge_nc_nofence / _nc_norotation / ct_bridge_negctl, ct_bridge_tc_driverstall / _tc_conductorstall
#   (ct_bridge_nc_mtu1408, the R-K6 wire RED, is retired: review NOTE-2, contract 19.5)
#
# The domain, codec and app seam flavours are chosen by each final target (production or seamed), never by a library,
# so a binary holds exactly one copy of each.

set(CT_BRIDGE_PURE_SOURCES BridgeConfig.cpp BridgeMetricsRender.cpp AppSeams.cpp)

add_library(ct_bridge_pure STATIC ${CT_BRIDGE_PURE_SOURCES})
target_include_directories(ct_bridge_pure PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ct_bridge_pure PUBLIC ct_app_common ct_client_core ct_bridge_domain ct_archive_core)
target_compile_options(ct_bridge_pure PRIVATE -Wall -Wextra)

if(BUILD_TESTING)
    add_library(ct_bridge_pure_seamed STATIC ${CT_BRIDGE_PURE_SOURCES})
    target_include_directories(ct_bridge_pure_seamed PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(ct_bridge_pure_seamed PUBLIC CT_TEST_SEAMS=1)
    target_link_libraries(ct_bridge_pure_seamed PUBLIC ct_app_common ct_client_core ct_bridge_domain_seamed)
    target_compile_options(ct_bridge_pure_seamed PRIVATE -Wall -Wextra)

    add_executable(ct_bridge_config_tests
        tests/PureTestMain.cpp
        tests/BridgeConfigTest.cpp
        tests/MetricsRenderTest.cpp
        tests/AckMapTest.cpp)
    target_link_libraries(ct_bridge_config_tests PRIVATE ct_bridge_pure_seamed)
    target_include_directories(ct_bridge_config_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
    target_compile_definitions(ct_bridge_config_tests PRIVATE
        CT_BRIDGE_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden"
        CT_BRIDGE_ALERTS_FILE="${CMAKE_CURRENT_SOURCE_DIR}/alerts.yml")
    target_compile_options(ct_bridge_config_tests PRIVATE -Wall -Wextra)
    add_test(NAME bridge.config COMMAND ct_bridge_config_tests)
    add_test(NAME bridge.config_negative_controls
        COMMAND bash "${PROJECT_SOURCE_DIR}/archive/tests/run-negative-controls.sh" "$<TARGET_FILE:ct_bridge_config_tests>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/negative-controls-pure.txt")
    set_tests_properties(bridge.config_negative_controls PROPERTIES TIMEOUT 600)

    add_executable(ct_bridge_main_tests tests/MainTest.cpp tests/ConductorWatchTest.cpp)
    target_link_libraries(ct_bridge_main_tests PRIVATE ct_bridge_pure_seamed)
    target_include_directories(ct_bridge_main_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
    target_compile_options(ct_bridge_main_tests PRIVATE -Wall -Wextra)
    add_test(NAME bridge.main COMMAND ct_bridge_main_tests)
    set_tests_properties(bridge.main PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
    add_test(NAME bridge.main_negative_controls
        COMMAND bash "${PROJECT_SOURCE_DIR}/archive/tests/run-negative-controls.sh" "$<TARGET_FILE:ct_bridge_main_tests>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/negative-controls-main.txt")
    set_tests_properties(bridge.main_negative_controls PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)
endif()

if(WITH_AERON AND WITH_BRIDGE)
    if(NOT CT_BUILD_MEDIA_DRIVER)
        message(FATAL_ERROR "ct-bridge (WITH_BRIDGE) needs CT_BUILD_MEDIA_DRIVER=ON: the embedded driver mode links aeron_driver_static")
    endif()
    # The embedded driver: app/common/EmbeddedDriver.cpp (one copy with ct-forwarder, RC2 S10' 8.2 rule 4; compiled into
    # each final target, never a library, so the link order below holds) over aeron_driver_static of the verified tree (it carries the C client too; it is linked first so
    # the client symbols resolve from it and nothing of aeron_static is pulled twice). The ldd allowlist gains nothing.
    function(ct_bridge_binary name pure domain codec archive)
        add_executable(${name} main.cpp ${CT_EMBEDDED_DRIVER_SOURCE})
        target_link_libraries(${name} PRIVATE aeron_driver_static ${pure} ct_app_common_aeron ${codec} ${domain} ${archive}
            ct_client_aeron ct_sbe_codecs)
        target_compile_options(${name} PRIVATE -Wall -Wextra)
        add_dependencies(${name} ct_sbe_generate)
        if(DEFINED CT_BRIDGE_VERSION_STRING)
            target_compile_definitions(${name} PRIVATE CT_BRIDGE_VERSION="${CT_BRIDGE_VERSION_STRING}")
        endif()
    endfunction()
    ct_bridge_binary(ct-bridge ct_bridge_pure ct_bridge_domain ct_bridge_codec ct_archive_aeron)

    if(BUILD_TESTING)
        set(CT_BRIDGE_TEST_INCLUDES "${PROJECT_SOURCE_DIR}/third_party" "${PROJECT_SOURCE_DIR}/client/tests"
            "${PROJECT_SOURCE_DIR}/adapters/sbe/tests" "${PROJECT_SOURCE_DIR}/domain/bridge/tests"
            "${PROJECT_SOURCE_DIR}/archive/tests" "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        add_executable(ct_bridge_app_tests tests/AppTestMain.cpp tests/BridgeAgentTest.cpp tests/AgentPropertyTest.cpp
            tests/StorageStarvationAppTest.cpp
            tests/CodecAppTest.cpp tests/StopPathTest.cpp)
        target_link_libraries(ct_bridge_app_tests PRIVATE ct_bridge_pure_seamed ct_bridge_codec_seamed ct_bridge_domain_seamed
            ct_client ct_sbe_codecs)
        add_dependencies(ct_bridge_app_tests ct_sbe_generate)
        target_include_directories(ct_bridge_app_tests PRIVATE ${CT_BRIDGE_TEST_INCLUDES})
        target_compile_definitions(ct_bridge_app_tests PRIVATE
            CT_MEJOURNAL_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/archive/tests/fixtures/mejournal")
        target_compile_options(ct_bridge_app_tests PRIVATE -Wall -Wextra)
        add_test(NAME bridge.app COMMAND ct_bridge_app_tests)
        set_tests_properties(bridge.app PROPERTIES TIMEOUT 1800 ENVIRONMENT "CT_BRIDGE_AGENT_PROPERTY_RUNS=2000")
        add_test(NAME bridge.app_negative_controls
            COMMAND bash "${PROJECT_SOURCE_DIR}/archive/tests/run-negative-controls.sh" "$<TARGET_FILE:ct_bridge_app_tests>"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/negative-controls.txt")
        set_tests_properties(bridge.app_negative_controls PROPERTIES TIMEOUT 3600
            ENVIRONMENT "CT_BRIDGE_AGENT_PROPERTY_RUNS=200")

        add_executable(ct_bridge_app_alloc_tests tests/ZeroAllocAppTest.cpp)
        target_link_libraries(ct_bridge_app_alloc_tests PRIVATE ct_bridge_pure_seamed ct_bridge_codec_seamed
            ct_bridge_domain_seamed ct_client ct_sbe_codecs)
        add_dependencies(ct_bridge_app_alloc_tests ct_sbe_generate)
        target_include_directories(ct_bridge_app_alloc_tests PRIVATE ${CT_BRIDGE_TEST_INCLUDES})
        target_compile_definitions(ct_bridge_app_alloc_tests PRIVATE
            CT_MEJOURNAL_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/archive/tests/fixtures/mejournal")
        target_compile_options(ct_bridge_app_alloc_tests PRIVATE -Wall -Wextra)
        target_link_options(ct_bridge_app_alloc_tests PRIVATE -rdynamic)
        add_test(NAME bridge.zero_alloc_app COMMAND ct_bridge_app_alloc_tests)

        add_executable(ct_bridge_main_aeron_tests tests/MainAeronTest.cpp ${CT_EMBEDDED_DRIVER_SOURCE})
        target_link_libraries(ct_bridge_main_aeron_tests PRIVATE aeron_driver_static ct_bridge_pure_seamed ct_app_common_aeron
            ct_client_aeron)
        target_include_directories(ct_bridge_main_aeron_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
        target_compile_definitions(ct_bridge_main_aeron_tests PRIVATE
            CT_BRIDGE_BINARY="$<TARGET_FILE:ct-bridge>"
            CT_AERONMD_BINARY="$<TARGET_FILE:aeronmd>")
        add_dependencies(ct_bridge_main_aeron_tests ct-bridge aeronmd)
        if(CT_BRIDGE_TEST_BUILDS)
            target_compile_definitions(ct_bridge_main_aeron_tests PRIVATE
                CT_BRIDGE_TC_CONDUCTORSTALL_BINARY="$<TARGET_FILE:ct_bridge_tc_conductorstall>")
            add_dependencies(ct_bridge_main_aeron_tests ct_bridge_tc_conductorstall)
        endif()
        target_compile_options(ct_bridge_main_aeron_tests PRIVATE -Wall -Wextra)
        add_test(NAME bridge.main_aeron COMMAND ct_bridge_main_aeron_tests)
        set_tests_properties(bridge.main_aeron PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)
        add_test(NAME bridge.main_aeron_negative_controls
            COMMAND bash "${PROJECT_SOURCE_DIR}/archive/tests/run-negative-controls.sh"
                "$<TARGET_FILE:ct_bridge_main_aeron_tests>" "${CMAKE_CURRENT_SOURCE_DIR}/tests/negative-controls-main-aeron.txt")
        set_tests_properties(bridge.main_aeron_negative_controls PROPERTIES TIMEOUT 900 RUN_SERIAL TRUE)

        if(CT_HAVE_FEED_HARNESS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/it/BridgeE3AppTest.cpp")
            add_executable(ct_bridge_e3_app_tests it/BridgeE3AppTest.cpp)
            target_link_libraries(ct_bridge_e3_app_tests PRIVATE ct_bridge_pure_seamed ct_bridge_codec_seamed
                ct_bridge_domain_seamed ct_client ct_sbe_codecs)
            add_dependencies(ct_bridge_e3_app_tests ct_sbe_generate)
            target_include_directories(ct_bridge_e3_app_tests PRIVATE ${CT_BRIDGE_TEST_INCLUDES}
                "${PROJECT_SOURCE_DIR}/adapters/sbe/tests/oracle")
            target_compile_definitions(ct_bridge_e3_app_tests PRIVATE CT_FEED_HARNESS_JAR="${CT_FEED_HARNESS_JAR}"
                CT_JAVA="${Java_JAVA_EXECUTABLE}"
                CT_MEJOURNAL_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/archive/tests/fixtures/mejournal")
            target_compile_options(ct_bridge_e3_app_tests PRIVATE -Wall -Wextra)
            add_test(NAME bridge.e3_app COMMAND ct_bridge_e3_app_tests)
            set_tests_properties(bridge.e3_app PROPERTIES TIMEOUT 3600 ENVIRONMENT "CT_BRIDGE_E3_APP_STORIES=100")
            add_test(NAME bridge.e3_app_negative_controls
                COMMAND bash "${PROJECT_SOURCE_DIR}/archive/tests/run-negative-controls.sh"
                    "$<TARGET_FILE:ct_bridge_e3_app_tests>" "${CMAKE_CURRENT_SOURCE_DIR}/it/negative-controls-e3.txt")
            set_tests_properties(bridge.e3_app_negative_controls PROPERTIES TIMEOUT 3600
                ENVIRONMENT "CT_BRIDGE_E3_APP_STORIES=10")
        endif()

        # 12.7 E4 parity: every Java bridge @Test has a PARITY.md row (needs the assets tree at the money pin)
        set(CT_PARITY_ASSETS_TREE "" CACHE PATH "assets checkout at money_schema_ref (bridge.parity reads assets-bridge/src/test)")
        if(EXISTS "${CT_PARITY_ASSETS_TREE}/assets-bridge/src/test")
            add_test(NAME bridge.parity COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/it/parity-check.sh"
                "${CT_PARITY_ASSETS_TREE}" "${PROJECT_SOURCE_DIR}/domain/bridge/tests/PARITY.md"
                "${PROJECT_SOURCE_DIR}/archive/tests/PARITY.md")
        endif()
    endif()

    # 12.4 T1-BR: the drills on real clusters (the runner is also run drill by drill on the build box)
    if(CT_INTEGRATION_TESTS AND CT_BRIDGE_TEST_BUILDS AND EXISTS "${CT_IT_ASSETS_JAR}" AND CT_HAVE_FEED_HARNESS)
        add_test(NAME bridge.integration
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/it/run-bridge-it.sh" "${CMAKE_BINARY_DIR}" "${CT_IT_ASSETS_JAR}"
                "${CT_FEED_HARNESS_JAR}" all)
        set_tests_properties(bridge.integration PROPERTIES TIMEOUT 14400 RUN_SERIAL TRUE)
    endif()

    if(CT_BRIDGE_TEST_BUILDS)
        ct_bridge_binary(ct_bridge_nc_nofence ct_bridge_pure_seamed ct_bridge_domain_seamed ct_bridge_codec_seamed
            ct_archive_aeron_seamed)
        target_compile_definitions(ct_bridge_nc_nofence PRIVATE CT_BRIDGE_NC_PRESET="FENCE_OFF,REGISTRATION_GATE_OFF")
        ct_bridge_binary(ct_bridge_nc_norotation ct_bridge_pure_seamed ct_bridge_domain_seamed ct_bridge_codec_seamed
            ct_archive_aeron_seamed)
        target_compile_definitions(ct_bridge_nc_norotation PRIVATE CT_BRIDGE_NC_PRESET="ROTATION_OFF")
        ct_bridge_binary(ct_bridge_negctl ct_bridge_pure_seamed ct_bridge_domain_seamed ct_bridge_codec_seamed
            ct_archive_aeron_seamed)
        target_compile_definitions(ct_bridge_negctl PRIVATE CT_BRIDGE_NC_PRESET="NEGCTL_DROP_RECORDS_2_3")
        # RC2 S10' Q1: the tmpfs-starved-nc RED (storage starvation never ends the process)
        ct_bridge_binary(ct_bridge_nc_storagekeep ct_bridge_pure_seamed ct_bridge_domain_seamed ct_bridge_codec_seamed
            ct_archive_aeron_seamed)
        target_compile_definitions(ct_bridge_nc_storagekeep PRIVATE CT_BRIDGE_NC_PRESET="STORAGE_STARVED_EXIT_OFF")
        ct_bridge_binary(ct_bridge_tc_driverstall ct_bridge_pure_seamed ct_bridge_domain_seamed ct_bridge_codec_seamed
            ct_archive_aeron_seamed)
        target_compile_definitions(ct_bridge_tc_driverstall PRIVATE CT_BRIDGE_TC_DRIVERSTALL=1)
        ct_bridge_binary(ct_bridge_tc_conductorstall ct_bridge_pure_seamed ct_bridge_domain_seamed ct_bridge_codec_seamed
            ct_archive_aeron_seamed)
        target_compile_definitions(ct_bridge_tc_conductorstall PRIVATE CT_BRIDGE_TC_CONDUCTORSTALL=1)
    endif()
endif()
