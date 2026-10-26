# client/it - real-cluster integration scenarios (built with WITH_AERON; run by run-it.sh,
# which needs java and the engine jars). Registered as a ctest only with CT_INTEGRATION_TESTS=ON
# and CT_IT_ASSETS_JAR / CT_IT_MATCH_JAR set, so the default test run stays hermetic.
add_executable(ct_client_it ClientIntegration.cpp)
target_link_libraries(ct_client_it PRIVATE ct_client_aeron)
target_compile_options(ct_client_it PRIVATE -Wall -Wextra)

set(CT_IT_ASSETS_JAR "" CACHE FILEPATH "assets-cluster.jar at money_schema_ref (integration tests)")
set(CT_IT_MATCH_JAR "" CACHE FILEPATH "match-cluster.jar at match_schema_ref (integration tests)")
if(CT_INTEGRATION_TESTS)
    if(NOT EXISTS "${CT_IT_ASSETS_JAR}" OR NOT EXISTS "${CT_IT_MATCH_JAR}")
        message(FATAL_ERROR "CT_INTEGRATION_TESTS=ON needs -DCT_IT_ASSETS_JAR and -DCT_IT_MATCH_JAR")
    endif()
    add_test(NAME client.integration
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run-it.sh" "${CMAKE_BINARY_DIR}" "${CT_IT_ASSETS_JAR}" "${CT_IT_MATCH_JAR}")
    set_tests_properties(client.integration PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)
endif()
