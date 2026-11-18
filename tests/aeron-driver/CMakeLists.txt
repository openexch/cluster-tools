# tests/aeron-driver (WITH_AERON, CT_BUILD_MEDIA_DRIVER, BUILD_TESTING) - the Aeron C media driver as this repository
# builds it: the verified tarball plus the patches dependencies.lock names (aeron_patches; cmake/VerifiedDependencies.cmake
# applies them). The same aeron_driver_static archive is in aeronmd's libaeron_driver.so and in every embedded driver.
#
#   ct_aeron_driver_tests            linked against the patched driver archive
#                                    -> aeron_driver.elicit_setup_retry
#   ct_aeron_driver_unpatched_tests  the RED control: the same test with the dispatcher translation unit compiled from
#                                    the PRISTINE tarball file (extracted here from the verified tarball), linked ahead of
#                                    the archive, so the archive's patched member is never pulled
#                                    -> aeron_driver.elicit_setup_retry_unpatched_control: must FAIL with "no image formed"

set(CT_AERON_PRISTINE_DIR "${CMAKE_CURRENT_BINARY_DIR}/pristine")
set(CT_AERON_DISPATCHER_REL "aeron-driver/src/main/c/aeron_data_packet_dispatcher.c")
file(REMOVE_RECURSE "${CT_AERON_PRISTINE_DIR}")
file(ARCHIVE_EXTRACT INPUT "${AERON_TARBALL}" DESTINATION "${CT_AERON_PRISTINE_DIR}" PATTERNS "*/${CT_AERON_DISPATCHER_REL}")
file(GLOB CT_AERON_PRISTINE_DISPATCHER "${CT_AERON_PRISTINE_DIR}/*/${CT_AERON_DISPATCHER_REL}")
if(NOT EXISTS "${CT_AERON_PRISTINE_DISPATCHER}")
    message(FATAL_ERROR "tests/aeron-driver: ${CT_AERON_DISPATCHER_REL} is not in ${AERON_TARBALL}")
endif()

# The pristine dispatcher, compiled as the driver archive compiles its sources (its definitions and include dirs).
add_library(ct_aeron_pristine_dispatcher OBJECT "${CT_AERON_PRISTINE_DISPATCHER}")
target_compile_definitions(ct_aeron_pristine_dispatcher PRIVATE
    "$<TARGET_PROPERTY:aeron_driver_static,COMPILE_DEFINITIONS>")
target_include_directories(ct_aeron_pristine_dispatcher PRIVATE
    "$<TARGET_PROPERTY:aeron_driver_static,INCLUDE_DIRECTORIES>")
set_target_properties(ct_aeron_pristine_dispatcher PROPERTIES C_STANDARD 11)

foreach(flavour IN ITEMS patched unpatched)
    if(flavour STREQUAL "patched")
        set(name ct_aeron_driver_tests)
        add_executable(${name} ElicitSetupRetryTest.cpp)
    else()
        set(name ct_aeron_driver_unpatched_tests)
        add_executable(${name} ElicitSetupRetryTest.cpp $<TARGET_OBJECTS:ct_aeron_pristine_dispatcher>)
    endif()
    target_link_libraries(${name} PRIVATE aeron_driver_static)
    target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
    target_compile_options(${name} PRIVATE -Wall -Wextra)
endforeach()

add_test(NAME aeron_driver.elicit_setup_retry COMMAND ct_aeron_driver_tests)
set_tests_properties(aeron_driver.elicit_setup_retry PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
add_test(NAME aeron_driver.elicit_setup_retry_unpatched_control
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/check-unpatched-control.sh" "$<TARGET_FILE:ct_aeron_driver_unpatched_tests>"
        "${CT_AERON_PRISTINE_DISPATCHER}" "${AERON_SRC}/${CT_AERON_DISPATCHER_REL}" "no image formed")
set_tests_properties(aeron_driver.elicit_setup_retry_unpatched_control PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
