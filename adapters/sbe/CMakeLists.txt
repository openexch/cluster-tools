# adapters/sbe - codec generation wiring plus the one compiled match-codec adapter. No
# generated code is committed. One output directory per schema, each regenerated whole:
#   cluster  - the Aeron cluster session protocol (schema 111, from the Aeron tree configure
#              extracted from the verified tarball; WITH_AERON only)
#   money    - the core money-schema (schema 2, MONEY_SCHEMA; v7)
#   match    - the ME order-schema (schema 1, MATCH_SCHEMA; v12)
#   journal  - the ME settlement journal (schema 4, ME_JOURNAL_SCHEMA; v1)
# ct_sbe_codecs exposes the include paths plus build_info.h (pins + schema hashes).

set(CT_SBE_GEN_DIR "${CMAKE_BINARY_DIR}/generated/sbe")

set(CT_SBE_STAMPS)
set(CT_SBE_INCLUDES)
function(ct_generate name schema)
    set(out "${CT_SBE_GEN_DIR}/${name}")
    set(stamp "${CT_SBE_GEN_DIR}/${name}.stamp")
    add_custom_command(
        OUTPUT "${stamp}"
        COMMAND ${CMAKE_COMMAND} -E env
            "JAVA=${Java_JAVA_EXECUTABLE}" "SBE_CLASSPATH=${CT_SBE_VERIFIED_CLASSPATH}"
            "SBE_JAR_SHA256S=${CT_SBE_JAR_SHA256S}"
            bash "${CMAKE_CURRENT_SOURCE_DIR}/gen-codecs.sh" "${out}" "${stamp}" "${schema}"
        DEPENDS "${schema}" "${CMAKE_CURRENT_SOURCE_DIR}/gen-codecs.sh" ${CT_SBE_VERIFIED_JARS}
        COMMENT "sbe ${CT_LOCK_SBE_VERSION}: generating C++ codecs for ${name} (${schema})"
        VERBATIM)
    set(CT_SBE_STAMPS ${CT_SBE_STAMPS} "${stamp}" PARENT_SCOPE)
    set(CT_SBE_INCLUDES ${CT_SBE_INCLUDES} "${out}" PARENT_SCOPE)
endfunction()

if(WITH_AERON)
    set(CT_CLUSTER_SCHEMA "${AERON_SRC}/aeron-cluster/src/main/resources/cluster/aeron-cluster-codecs.xml")
    if(NOT EXISTS "${CT_CLUSTER_SCHEMA}")
        message(FATAL_ERROR "cluster codec schema not found: ${CT_CLUSTER_SCHEMA}")
    endif()
    ct_generate(cluster "${CT_CLUSTER_SCHEMA}")
endif()
ct_generate(money "${MONEY_SCHEMA}")
ct_generate(match "${MATCH_SCHEMA}")
ct_generate(journal "${ME_JOURNAL_SCHEMA}")
set(CT_HAVE_JOURNAL 1)

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/build_info.h.in"
    "${CT_SBE_GEN_DIR}/info/cluster_tools/build_info.h" @ONLY)

add_custom_target(ct_sbe_generate DEPENDS ${CT_SBE_STAMPS})

add_library(ct_sbe_codecs INTERFACE)
add_library(cluster_tools::sbe_codecs ALIAS ct_sbe_codecs)
add_dependencies(ct_sbe_codecs ct_sbe_generate)
target_include_directories(ct_sbe_codecs SYSTEM INTERFACE ${CT_SBE_INCLUDES} "${CT_SBE_GEN_DIR}/info")

add_library(ct_adapter_sbe STATIC
    src/EngineConfigCodec.cpp)
add_library(cluster_tools::adapter_sbe ALIAS ct_adapter_sbe)
add_dependencies(ct_adapter_sbe ct_sbe_generate)
target_include_directories(ct_adapter_sbe PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ct_adapter_sbe
    PUBLIC ct_domain ct_ports
    PRIVATE ct_sbe_codecs)
target_compile_options(ct_adapter_sbe PRIVATE -Wall -Wextra)

# RC2 S12 W5: the forwarder's wire (money 104/105/108/109/101 decode, 65 encode; order 9/10 encode, 21 decode).
# The domain value types come from domain/forwarder (headers only): consumers link the domain flavour they
# want (production or seamed), so the codec never pulls a second copy in.
add_library(ct_forwarder_codec STATIC src/ForwarderCodec.cpp)
add_library(cluster_tools::forwarder_codec ALIAS ct_forwarder_codec)
add_dependencies(ct_forwarder_codec ct_sbe_generate)
target_include_directories(ct_forwarder_codec PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/domain/forwarder/include")
target_link_libraries(ct_forwarder_codec PRIVATE ct_sbe_codecs)
target_compile_options(ct_forwarder_codec PRIVATE -Wall -Wextra)

# RC2 S9b section 5: the bridge's wire (schema 4 records -> money v7 feed 61-64, 67, 68; FeedHalt 66, Query 7; the
# egress 17 v7, 110, 114). The domain's value types and ports come from domain/bridge (headers), the frame shapes
# from archive (FramePeek, header-only constants); consumers link the domain flavour they want.
add_library(ct_bridge_codec STATIC src/BridgeCodec.cpp)
add_library(cluster_tools::bridge_codec ALIAS ct_bridge_codec)
add_dependencies(ct_bridge_codec ct_sbe_generate)
target_include_directories(ct_bridge_codec PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/domain/bridge/include" "${PROJECT_SOURCE_DIR}/archive/include")
target_link_libraries(ct_bridge_codec PRIVATE ct_sbe_codecs)
target_compile_options(ct_bridge_codec PRIVATE -Wall -Wextra)

if(BUILD_TESTING)
    # the codec's negative-control seams (CODEC_*), resolved by ct_bridge_domain_seamed's TestSeams
    add_library(ct_bridge_codec_seamed STATIC src/BridgeCodec.cpp)
    add_dependencies(ct_bridge_codec_seamed ct_sbe_generate)
    target_include_directories(ct_bridge_codec_seamed PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_definitions(ct_bridge_codec_seamed PUBLIC CT_TEST_SEAMS=1)
    target_link_libraries(ct_bridge_codec_seamed PUBLIC ct_bridge_domain_seamed PRIVATE ct_sbe_codecs)
    target_compile_options(ct_bridge_codec_seamed PRIVATE -Wall -Wextra)

    add_executable(ct_bridge_codec_tests tests/TestMain.cpp tests/BridgeCodecTest.cpp)
    target_link_libraries(ct_bridge_codec_tests PRIVATE ct_bridge_codec_seamed ct_bridge_domain_seamed ct_sbe_codecs)
    target_include_directories(ct_bridge_codec_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party" "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_compile_definitions(ct_bridge_codec_tests PRIVATE
        CT_E1_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/domain/bridge/tests/fixtures/e1"
        CT_MEJOURNAL_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/archive/tests/fixtures/mejournal")
    target_compile_options(ct_bridge_codec_tests PRIVATE -Wall -Wextra)
    add_test(NAME bridge.codec COMMAND ct_bridge_codec_tests)
    add_test(NAME bridge.codec_negative_controls
        COMMAND bash "${PROJECT_SOURCE_DIR}/archive/tests/run-negative-controls.sh" "$<TARGET_FILE:ct_bridge_codec_tests>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/bridge-codec-negative-controls.txt")
    set_tests_properties(bridge.codec_negative_controls PROPERTIES TIMEOUT 600)

    if(CT_HAVE_FEED_HARNESS)
        # 13.12 E2: the harness's `random` against the C++ translate (10^5 records on the dev box, 10^6 in CI via
        # CT_DIFF_COUNT); the seed is printed in every run
        add_executable(ct_bridge_differential_tests tests/TestMain.cpp tests/TranslatorDifferentialTest.cpp)
        target_link_libraries(ct_bridge_differential_tests PRIVATE ct_bridge_codec_seamed ct_bridge_domain_seamed ct_sbe_codecs)
        target_include_directories(ct_bridge_differential_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        target_compile_definitions(ct_bridge_differential_tests PRIVATE CT_JAVA="${Java_JAVA_EXECUTABLE}"
            CT_FEED_HARNESS_JAR="${CT_FEED_HARNESS_JAR}")
        target_compile_options(ct_bridge_differential_tests PRIVATE -Wall -Wextra)
        add_test(NAME bridge.e2_differential COMMAND ct_bridge_differential_tests)
        set_tests_properties(bridge.e2_differential PROPERTIES TIMEOUT 1200)
        add_test(NAME bridge.e2_differential_negative_controls
            COMMAND bash "${PROJECT_SOURCE_DIR}/archive/tests/run-negative-controls.sh"
                "$<TARGET_FILE:ct_bridge_differential_tests>"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/bridge-differential-negative-controls.txt")
        set_tests_properties(bridge.e2_differential_negative_controls PROPERTIES TIMEOUT 1200
            ENVIRONMENT "CT_DIFF_COUNT=20000")

        # 13.17 E3 foundation: the real BridgeDomain + BridgeCodec over SimArchive sources feed the real core (`serve`)
        add_executable(ct_bridge_oracle_tests tests/TestMain.cpp tests/oracle/RealCoreOracleTest.cpp)
        target_link_libraries(ct_bridge_oracle_tests PRIVATE ct_bridge_codec_seamed ct_bridge_domain_seamed ct_sbe_codecs)
        target_include_directories(ct_bridge_oracle_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests" "${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle"
            "${PROJECT_SOURCE_DIR}/archive/tests")
        target_compile_definitions(ct_bridge_oracle_tests PRIVATE CT_JAVA="${Java_JAVA_EXECUTABLE}"
            CT_MEJOURNAL_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/archive/tests/fixtures/mejournal"
            CT_FEED_HARNESS_JAR="${CT_FEED_HARNESS_JAR}")
        target_compile_options(ct_bridge_oracle_tests PRIVATE -Wall -Wextra)
        add_test(NAME bridge.e3_real_core_oracle COMMAND ct_bridge_oracle_tests)
        set_tests_properties(bridge.e3_real_core_oracle PROPERTIES TIMEOUT 1800 ENVIRONMENT "CT_ORACLE_RUNS=10")
        add_test(NAME bridge.e3_real_core_oracle_negative_controls
            COMMAND bash "${PROJECT_SOURCE_DIR}/archive/tests/run-negative-controls.sh"
                "$<TARGET_FILE:ct_bridge_oracle_tests>"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/negative-controls.txt")
        set_tests_properties(bridge.e3_real_core_oracle_negative_controls PROPERTIES TIMEOUT 1800
            ENVIRONMENT "CT_ORACLE_RUNS=3")
    else()
        message(STATUS "RC2 S9b: no CT_FEED_HARNESS_JAR: the E2 differential and the real-core oracle are not built")
    endif()

    add_executable(ct_forwarder_codec_tests tests/ForwarderCodecTest.cpp)
    # RC2 S13 W1: the seamed domain flavour, so the 4.2 frame-validation rules (MarketData.cpp) can be switched off
    target_link_libraries(ct_forwarder_codec_tests PRIVATE ct_forwarder_codec ct_forwarder_domain_seamed ct_sbe_codecs)
    target_include_directories(ct_forwarder_codec_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party" "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_compile_options(ct_forwarder_codec_tests PRIVATE -Wall -Wextra)
    add_test(NAME forwarder.codec COMMAND ct_forwarder_codec_tests)
    add_test(NAME forwarder.codec_negative_controls
        COMMAND bash "${PROJECT_SOURCE_DIR}/archive/tests/run-negative-controls.sh" "$<TARGET_FILE:ct_forwarder_codec_tests>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/forwarder-codec-negative-controls.txt")
    set_tests_properties(forwarder.codec_negative_controls PROPERTIES TIMEOUT 300)

    # Codec layout pins: the generated money/match codecs must have the block lengths and
    # template ids the Java engines encode (golden bytes in the test).
    add_executable(ct_codec_tests tests/CodecLayoutTest.cpp)
    target_link_libraries(ct_codec_tests PRIVATE ct_sbe_codecs)
    target_include_directories(ct_codec_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
    target_compile_options(ct_codec_tests PRIVATE -Wall -Wextra)
    add_test(NAME adapters.sbe_codec_layout COMMAND ct_codec_tests)

    # EngineConfigCodec against the ME's own Java encoder (v12 root fields included).
    add_executable(ct_engine_config_codec_tests tests/EngineConfigCodecTest.cpp)
    target_link_libraries(ct_engine_config_codec_tests PRIVATE ct_adapter_sbe ct_sbe_codecs)
    target_include_directories(ct_engine_config_codec_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
    target_compile_options(ct_engine_config_codec_tests PRIVATE -Wall -Wextra)
    add_test(NAME adapters.sbe_engine_config_codec COMMAND ct_engine_config_codec_tests)

    # The verified-dependency gate refuses tampered/unpinned inputs (scripts/check-configure-gate.sh).
    if(WITH_AERON)
    add_test(NAME build.configure_gate
        COMMAND bash "${PROJECT_SOURCE_DIR}/scripts/check-configure-gate.sh" "${CMAKE_COMMAND}"
            "${AERON_TARBALL}" "${SBE_CLASSPATH}" "${MONEY_SCHEMA}" "${MATCH_SCHEMA}" "${ME_JOURNAL_SCHEMA}"
            ${CT_IT_AERON_ALL_JAR})
    set_tests_properties(build.configure_gate PROPERTIES TIMEOUT 300
        ENVIRONMENT "CT_GATE_FEED_HARNESS_JAR=${CT_FEED_HARNESS_JAR}")
    endif()
endif()
