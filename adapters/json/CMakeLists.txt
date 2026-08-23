# adapters/json — the engine-config file input adapter. Hand-rolled, schema-directed,
# zero dependencies; unlike the transport adapters it needs no aeron checkout, so it
# builds (and its tests run) in the WITH_AERON=OFF configuration, CI included.

add_library(ct_adapter_json STATIC
    src/EngineConfigJson.cpp)
add_library(cluster_tools::adapter_json ALIAS ct_adapter_json)

target_include_directories(ct_adapter_json PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ct_adapter_json PUBLIC ct_domain ct_ports)
target_compile_options(ct_adapter_json PRIVATE -Wall -Wextra)

if(BUILD_TESTING)
    add_executable(ct_adapter_json_tests tests/EngineConfigJsonTest.cpp)
    target_link_libraries(ct_adapter_json_tests PRIVATE ct_adapter_json)
    target_include_directories(ct_adapter_json_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
    target_compile_options(ct_adapter_json_tests PRIVATE -Wall -Wextra)
    add_test(NAME adapters.engine_config_json COMMAND ct_adapter_json_tests)
endif()
