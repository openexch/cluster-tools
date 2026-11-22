# app/genesis - ct-genesis, the genesis of an empty ME and core pair (one verb per phase, `status`, `verify`).
#
# Every configuration (the WITH_AERON=OFF fast job included):
#   ct_genesis_pure            the strict input file, the strict settings, the judgement of a state (GenesisState)
#   ct_genesis_pure_tests      GenesisPureTest (genesis.pure)
# WITH_CODECS:
#   ct_genesis_core / _seamed  the wire (money and match codecs) and the runner; the seamed flavour reads the
#                              negative-control seams (GenesisSeams.h)
#   ct_genesis_runner_tests    GenesisRunner over the in-process fake of both clusters (genesis.runner), and its REDs
#                              (genesis.runner_negative_controls, tests/negative-controls.txt)
# WITH_AERON (CT_BUILD_MEDIA_DRIVER: the embedded mode links aeron_driver_static):
#   ct-genesis                 the binary
# CT_GENESIS_TEST_BUILDS (never published):
#   ct_genesis_tc              ct-genesis with the seams and CT_GENESIS_TC_EXIT_AFTER_SENDS (the killed-genesis drill)
# CT_INTEGRATION_TESTS (CT_IT_CORE_JAR, CT_IT_MATCH_JAR) with CT_GENESIS_TEST_BUILDS and WITH_BRIDGE: genesis.integration (it/run-genesis-it.sh:
#   a 3-node core, a 3-node ME with its journal, ct-bridge, the tool in both driver modes, the engines on their embedded
#   driver and, in one leg, on external aeronmd; CT_IT_DRIVER_PROPERTIES names the pinned driver.properties)

set(CT_GENESIS_PURE_SOURCES Json.cpp GenesisPlan.cpp GenesisState.cpp GenesisSettings.cpp GenesisSeams.cpp)

add_library(ct_genesis_pure STATIC ${CT_GENESIS_PURE_SOURCES})
target_link_libraries(ct_genesis_pure PUBLIC ct_app_common ct_client_core ct_domain)
target_compile_options(ct_genesis_pure PRIVATE -Wall -Wextra)

add_library(ct_genesis_pure_seamed STATIC ${CT_GENESIS_PURE_SOURCES})
target_compile_definitions(ct_genesis_pure_seamed PUBLIC CT_TEST_SEAMS=1)
target_link_libraries(ct_genesis_pure_seamed PUBLIC ct_app_common ct_client_core ct_domain)
target_compile_options(ct_genesis_pure_seamed PRIVATE -Wall -Wextra)

if(BUILD_TESTING)
    add_executable(ct_genesis_pure_tests tests/GenesisPureTest.cpp)
    target_link_libraries(ct_genesis_pure_tests PRIVATE ct_genesis_pure)
    target_include_directories(ct_genesis_pure_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
    target_compile_definitions(ct_genesis_pure_tests PRIVATE
        CT_GENESIS_EXAMPLE="${CMAKE_CURRENT_SOURCE_DIR}/genesis.example.json")
    target_compile_options(ct_genesis_pure_tests PRIVATE -Wall -Wextra)
    add_test(NAME genesis.pure COMMAND ct_genesis_pure_tests)
endif()

if(WITH_CODECS)
    foreach(flavour IN ITEMS plain seamed)
        if(flavour STREQUAL "plain")
            set(name ct_genesis_core)
            set(pure ct_genesis_pure)
        else()
            set(name ct_genesis_core_seamed)
            set(pure ct_genesis_pure_seamed)
        endif()
        add_library(${name} STATIC GenesisWire.cpp GenesisRunner.cpp)
        add_dependencies(${name} ct_sbe_generate)
        target_link_libraries(${name} PUBLIC ${pure} ct_adapter_sbe PRIVATE ct_sbe_codecs)
        target_compile_options(${name} PRIVATE -Wall -Wextra)
    endforeach()

    if(BUILD_TESTING)
        add_executable(ct_genesis_runner_tests tests/GenesisRunnerTest.cpp)
        target_link_libraries(ct_genesis_runner_tests PRIVATE ct_genesis_core_seamed ct_sbe_codecs)
        target_include_directories(ct_genesis_runner_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        target_compile_definitions(ct_genesis_runner_tests PRIVATE
            CT_GENESIS_EXAMPLE="${CMAKE_CURRENT_SOURCE_DIR}/genesis.example.json")
        target_compile_options(ct_genesis_runner_tests PRIVATE -Wall -Wextra)
        add_test(NAME genesis.runner COMMAND ct_genesis_runner_tests)
        add_test(NAME genesis.runner_negative_controls
            COMMAND bash "${PROJECT_SOURCE_DIR}/archive/tests/run-negative-controls.sh"
                "$<TARGET_FILE:ct_genesis_runner_tests>" "${CMAKE_CURRENT_SOURCE_DIR}/tests/negative-controls.txt")
        set_tests_properties(genesis.runner_negative_controls PROPERTIES TIMEOUT 300)
    endif()
endif()

if(WITH_AERON)
    if(NOT CT_BUILD_MEDIA_DRIVER)
        message(FATAL_ERROR "ct-genesis (WITH_AERON) needs CT_BUILD_MEDIA_DRIVER=ON: the embedded driver mode links aeron_driver_static")
    endif()
    function(ct_genesis_binary name core)
        add_executable(${name} main.cpp ${CT_EMBEDDED_DRIVER_SOURCE})
        # the embedded driver first (runtime/CMakeLists.txt): its archive resolves the C client symbols before aeron_static
        target_link_libraries(${name} PRIVATE aeron_driver_static ${core} ct_runtime_aeron ct_client_aeron ct_client
            ct_sbe_codecs)
        target_compile_options(${name} PRIVATE -Wall -Wextra)
        target_compile_definitions(${name} PRIVATE CT_BUILD_SOURCE_SHA="${CT_SOURCE_SHA_EFFECTIVE}")
    endfunction()
    ct_genesis_binary(ct-genesis ct_genesis_core)
    if(CT_GENESIS_TEST_BUILDS)
        ct_genesis_binary(ct_genesis_tc ct_genesis_core_seamed)
    endif()

    # The engine jars of client/it (CT_IT_CORE_JAR, CT_IT_MATCH_JAR); the run needs ct-bridge and ct_genesis_tc
    # The external-engine leg launches every engine driver on the pinned driver.properties (the run checks its sha256)
    set(CT_IT_DRIVER_PROPERTIES "" CACHE FILEPATH
        "driver.properties at the lock's driver_properties_ref (scripts/stage-driver-properties.sh): the genesis integration run's external-engine leg")
    if(CT_INTEGRATION_TESTS AND CT_GENESIS_TEST_BUILDS AND WITH_BRIDGE)
        add_test(NAME genesis.integration
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/it/run-genesis-it.sh" "${CMAKE_BINARY_DIR}" "${CT_IT_CORE_JAR}"
                "${CT_IT_MATCH_JAR}")
        set_tests_properties(genesis.integration PROPERTIES TIMEOUT 3000 RUN_SERIAL TRUE
            ENVIRONMENT "IT_DRIVER_PROPERTIES=${CT_IT_DRIVER_PROPERTIES}")
    endif()
endif()
