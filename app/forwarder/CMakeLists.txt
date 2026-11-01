# app/forwarder - ct-forwarder, the RC2 S12 production forwarder.
#
# Every configuration (the WITH_AERON=OFF fast job included):
#   ct_forwarder_pure / _seamed  ForwarderConfig (strict env, the RC2 S10' driver keys), MetricsRender, the app seams
#                              (S10' 8.3); links ct_app_common (HttpServer, Supervisor, LogRing, StatusBoardT: moved to
#                              app/common by RC2 S9c W1)
#   ct_forwarder_config_tests  ForwarderConfigTest + MetricsRenderTest + TcCaptureTest (+ the S10' pure REDs, negative-controls-pure.txt)
#   ct_forwarder_main_tests    the watchdog, the 2 s join bound and the stop path against duty stubs (FIX-8)
# WITH_AERON (CT_BUILD_MEDIA_DRIVER: the embedded mode links aeron_driver_static, S10' section 8):
#   ct-forwarder               the published binary (publish.yml FILES + manifest)
#   ct_forwarder_app_tests     ForwarderAgent over two FakeTransports (contract 11.3) + the S10' identity test
#   ct_forwarder_main_aeron_tests  S10' 8.3: startSequenceBothModes, conductorStallExits70 (real drivers, the real binaries)
# CT_FORWARDER_TEST_BUILDS (never published; publish.yml refuses them):
#   ct_forwarder_nc_noresync / _nc_norelease / _nc_refuse  negative-control presets of 11.4
#   ct_forwarder_tc_corrupt                                 T1-G: a core defect simulated after decode
#   ct_forwarder_tc_conductorstall                          S10' 8.3: SIGUSR2 parks the Aeron client conductor
#   ct_forwarder_tc_capture                                 S10' 7.1 item 2: every ME frame offered -> FWD_CAPTURE_DIR
#
# The domain and app seam flavours are chosen by each final target (production or seamed), never by a library, so a
# binary holds exactly one copy of each.

set(CT_FORWARDER_PURE_SOURCES ForwarderConfig.cpp MetricsRender.cpp AppSeams.cpp)

add_library(ct_forwarder_pure STATIC ${CT_FORWARDER_PURE_SOURCES})
target_include_directories(ct_forwarder_pure PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/domain/forwarder/include")
target_link_libraries(ct_forwarder_pure PUBLIC ct_app_common ct_client_core Threads::Threads)
target_compile_options(ct_forwarder_pure PRIVATE -Wall -Wextra)

# the seamed flavour (RC2 S10' 8.3): the test targets and the nc / tc builds only
add_library(ct_forwarder_pure_seamed STATIC ${CT_FORWARDER_PURE_SOURCES})
target_include_directories(ct_forwarder_pure_seamed PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/domain/forwarder/include")
target_compile_definitions(ct_forwarder_pure_seamed PUBLIC CT_TEST_SEAMS=1)
target_link_libraries(ct_forwarder_pure_seamed PUBLIC ct_app_common ct_client_core Threads::Threads)
target_compile_options(ct_forwarder_pure_seamed PRIVATE -Wall -Wextra)

if(BUILD_TESTING)
    add_executable(ct_forwarder_config_tests
        tests/PureTestMain.cpp
        tests/ForwarderConfigTest.cpp
        tests/MetricsRenderTest.cpp
        tests/TcCaptureTest.cpp)
    target_link_libraries(ct_forwarder_config_tests PRIVATE ct_forwarder_pure_seamed ct_forwarder_domain)
    target_include_directories(ct_forwarder_config_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
    target_compile_definitions(ct_forwarder_config_tests PRIVATE
        CT_FORWARDER_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden"
        CT_FORWARDER_ALERTS_FILE="${CMAKE_CURRENT_SOURCE_DIR}/alerts.yml")
    target_compile_options(ct_forwarder_config_tests PRIVATE -Wall -Wextra)
    add_test(NAME forwarder.config COMMAND ct_forwarder_config_tests)
    add_test(NAME forwarder.config_negative_controls
        COMMAND bash "${PROJECT_SOURCE_DIR}/archive/tests/run-negative-controls.sh" "$<TARGET_FILE:ct_forwarder_config_tests>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/negative-controls-pure.txt")
    set_tests_properties(forwarder.config_negative_controls PROPERTIES TIMEOUT 300)

    add_executable(ct_forwarder_main_tests tests/MainTest.cpp)
    target_link_libraries(ct_forwarder_main_tests PRIVATE ct_forwarder_pure ct_forwarder_domain)
    target_include_directories(ct_forwarder_main_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
    target_compile_options(ct_forwarder_main_tests PRIVATE -Wall -Wextra)
    add_test(NAME forwarder.main COMMAND ct_forwarder_main_tests)
    set_tests_properties(forwarder.main PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
endif()

if(WITH_AERON)
    if(NOT CT_BUILD_MEDIA_DRIVER)
        message(FATAL_ERROR "ct-forwarder (WITH_AERON) needs CT_BUILD_MEDIA_DRIVER=ON: the embedded driver mode links aeron_driver_static (RC2 S10' section 8)")
    endif()
    # RC2 S10' 8.2 rule 4: the embedded driver is app/common's (the bridge's, moved): aeron_driver_static is linked first,
    # as in ct-bridge, so the client symbols resolve from it and nothing of aeron_static is pulled twice. The seam flavour
    # follows the domain flavour: a seamed domain means a test build, which takes the seamed app layer too.
    function(ct_forwarder_binary name domain)
        add_executable(${name} main.cpp ${CT_EMBEDDED_DRIVER_SOURCE})
        if(domain STREQUAL "ct_forwarder_domain")
            set(pure ct_forwarder_pure)
        else()
            set(pure ct_forwarder_pure_seamed)
        endif()
        target_link_libraries(${name} PRIVATE aeron_driver_static ${pure} ct_app_common_aeron ct_forwarder_codec ${domain}
            ct_client_aeron ct_sbe_codecs)
        target_compile_options(${name} PRIVATE -Wall -Wextra)
        if(DEFINED CT_FORWARDER_VERSION_STRING)
            target_compile_definitions(${name} PRIVATE CT_FORWARDER_VERSION="${CT_FORWARDER_VERSION_STRING}")
        endif()
    endfunction()
    ct_forwarder_binary(ct-forwarder ct_forwarder_domain)

    if(BUILD_TESTING)
        add_executable(ct_forwarder_app_tests tests/AppTestMain.cpp tests/ForwarderAgentTest.cpp tests/ZeroAllocTest.cpp
            tests/IdentityTest.cpp tests/StorageStarvationTest.cpp)
        target_link_libraries(ct_forwarder_app_tests PRIVATE ct_forwarder_pure_seamed ct_forwarder_codec ct_forwarder_domain_seamed
            ct_client ct_sbe_codecs)
        target_include_directories(ct_forwarder_app_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party"
            "${PROJECT_SOURCE_DIR}/client/tests" "${PROJECT_SOURCE_DIR}/adapters/sbe/tests")
        target_compile_options(ct_forwarder_app_tests PRIVATE -Wall -Wextra)
        add_test(NAME forwarder.app COMMAND ct_forwarder_app_tests)

        add_test(NAME forwarder.app_negative_controls
            COMMAND bash "${PROJECT_SOURCE_DIR}/archive/tests/run-negative-controls.sh" "$<TARGET_FILE:ct_forwarder_app_tests>"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/negative-controls.txt")
        set_tests_properties(forwarder.app_negative_controls PROPERTIES TIMEOUT 900)

        # RC2 S10' 8.3: the start sequence in both driver modes and the conductor stall, against the real binaries
        add_executable(ct_forwarder_main_aeron_tests tests/MainAeronTest.cpp ${CT_EMBEDDED_DRIVER_SOURCE})
        target_link_libraries(ct_forwarder_main_aeron_tests PRIVATE aeron_driver_static ct_forwarder_pure_seamed
            ct_app_common_aeron ct_client_aeron)
        target_include_directories(ct_forwarder_main_aeron_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
        target_compile_definitions(ct_forwarder_main_aeron_tests PRIVATE
            CT_FORWARDER_BINARY="$<TARGET_FILE:ct-forwarder>"
            CT_AERONMD_BINARY="$<TARGET_FILE:aeronmd>")
        add_dependencies(ct_forwarder_main_aeron_tests ct-forwarder aeronmd)
        if(CT_FORWARDER_TEST_BUILDS)
            target_compile_definitions(ct_forwarder_main_aeron_tests PRIVATE
                CT_FORWARDER_TC_CONDUCTORSTALL_BINARY="$<TARGET_FILE:ct_forwarder_tc_conductorstall>")
            add_dependencies(ct_forwarder_main_aeron_tests ct_forwarder_tc_conductorstall)
        endif()
        target_compile_options(ct_forwarder_main_aeron_tests PRIVATE -Wall -Wextra)
        add_test(NAME forwarder.main_aeron COMMAND ct_forwarder_main_aeron_tests)
        set_tests_properties(forwarder.main_aeron PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)
        add_test(NAME forwarder.main_aeron_negative_controls
            COMMAND bash "${PROJECT_SOURCE_DIR}/archive/tests/run-negative-controls.sh"
                "$<TARGET_FILE:ct_forwarder_main_aeron_tests>" "${CMAKE_CURRENT_SOURCE_DIR}/tests/negative-controls-main-aeron.txt")
        set_tests_properties(forwarder.main_aeron_negative_controls PROPERTIES TIMEOUT 900 RUN_SERIAL TRUE)
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
        # RC2 S10' 7.1 item 2 (T1-F re-sends): fwdA writes every template 9 / 10 frame it offers to the ME to
        # FWD_CAPTURE_DIR/<n>.bin plus a sidecar line with its SHA-256 (TcCapture.h; review N-11)
        ct_forwarder_binary(ct_forwarder_tc_capture ct_forwarder_domain_seamed)
        target_compile_definitions(ct_forwarder_tc_capture PRIVATE CT_TEST_SEAMS=1 CT_FORWARDER_TC_CAPTURE=1)
        # RC2 S9c 12.5 (T1-G negative control): nc_refuse needs corrupted input to refuse, so the T1-G RED runs the
        # refusal preset over the test corruption (never published: the publish guard refuses _nc_ and _tc_ names)
        ct_forwarder_binary(ct_forwarder_nc_refuse_tc ct_forwarder_domain_seamed)
        target_compile_definitions(ct_forwarder_nc_refuse_tc PRIVATE CT_TEST_SEAMS=1 CT_FORWARDER_NC_PRESET="AGENT_SEND_INVALID"
            CT_FORWARDER_TC_CORRUPT=1)
        # RC2 S10' 8.3 MainAeronTest.conductorStallExits70 (and its CONDUCTOR_WATCH_OFF RED through the env seam)
        ct_forwarder_binary(ct_forwarder_tc_conductorstall ct_forwarder_domain_seamed)
        target_compile_definitions(ct_forwarder_tc_conductorstall PRIVATE CT_TEST_SEAMS=1 CT_FORWARDER_TC_CONDUCTORSTALL=1)
        # RC2 S10' Q1: the storage-starved-nc RED of run-forwarder-it.sh (storage starvation never ends the process)
        ct_forwarder_binary(ct_forwarder_nc_storagekeep ct_forwarder_domain_seamed)
        target_compile_definitions(ct_forwarder_nc_storagekeep PRIVATE CT_TEST_SEAMS=1
            CT_FORWARDER_NC_PRESET="STORAGE_STARVED_EXIT_OFF")
    endif()
endif()
