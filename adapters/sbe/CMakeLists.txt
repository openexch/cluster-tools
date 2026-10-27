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

if(BUILD_TESTING)
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
    set_tests_properties(build.configure_gate PROPERTIES TIMEOUT 300)
    endif()
endif()
