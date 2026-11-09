# domain — pure logic. No Aeron, no SBE, no I/O; builds and tests anywhere a C++17
# compiler exists. If a change here needs a transport header, the change is wrong.

add_library(ct_domain STATIC
    src/SwitchDecision.cpp
    src/EngineConfig.cpp)
add_library(cluster_tools::domain ALIAS ct_domain)

target_include_directories(ct_domain PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_options(ct_domain PRIVATE -Wall -Wextra)

if(BUILD_TESTING)
    add_executable(ct_domain_tests tests/SwitchDecisionTest.cpp)
    target_link_libraries(ct_domain_tests PRIVATE ct_domain)
    target_include_directories(ct_domain_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
    target_compile_options(ct_domain_tests PRIVATE -Wall -Wextra)
    add_test(NAME domain.switch_decision COMMAND ct_domain_tests)

    add_executable(ct_domain_engine_config_tests tests/EngineConfigTest.cpp)
    target_link_libraries(ct_domain_engine_config_tests PRIVATE ct_domain)
    target_include_directories(ct_domain_engine_config_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
    target_compile_options(ct_domain_engine_config_tests PRIVATE -Wall -Wextra)
    add_test(NAME domain.engine_config COMMAND ct_domain_engine_config_tests)

    # Negative control: the same tests over a seamed copy of the library (CT_TEST_DISABLE_GENESIS_ID_POSITIVE reverts
    # the genesisId rule to "== 0 only"); never linked into a tool
    add_library(ct_domain_seamed STATIC src/SwitchDecision.cpp src/EngineConfig.cpp)
    target_include_directories(ct_domain_seamed PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_definitions(ct_domain_seamed PRIVATE CT_TEST_SEAMS=1)
    target_compile_options(ct_domain_seamed PRIVATE -Wall -Wextra)
    add_executable(ct_domain_engine_config_seamed_tests tests/EngineConfigTest.cpp)
    target_link_libraries(ct_domain_engine_config_seamed_tests PRIVATE ct_domain_seamed)
    target_include_directories(ct_domain_engine_config_seamed_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
    target_compile_options(ct_domain_engine_config_seamed_tests PRIVATE -Wall -Wextra)
    add_test(NAME domain.engine_config_negative_controls
        COMMAND bash "${PROJECT_SOURCE_DIR}/archive/tests/run-negative-controls.sh"
            "$<TARGET_FILE:ct_domain_engine_config_seamed_tests>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/negative-controls-engine-config.txt")
endif()
