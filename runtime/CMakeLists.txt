# runtime - the process machinery of a C++ cluster process, one copy for ct-forwarder, ct-bridge and every
# other repository that links the installed package (cmake/Package.cmake) instead of copying it:
#
#   cluster_tools::runtime                  (every configuration, pure POSIX) Supervisor (main's sigtimedwait loop,
#                                           duty watchdog, stop path, exit seam, NonBlockingLogWriter), HttpServer,
#                                           LogRing, StatusBoardT, IdleStrategy, DriverTimeout, DirProbe,
#                                           StorageStarvation; header-only parts plus one static library.
#   cluster_tools::runtime_aeron            (WITH_AERON) ConductorWatch, the client-conductor heartbeat check.
#   cluster_tools::runtime_embedded_driver  (WITH_AERON and CT_BUILD_MEDIA_DRIVER) the embedded media driver: an
#                                           interface target whose source (EmbeddedDriver.cpp) each final target
#                                           compiles itself and whose first link item is aeron_driver_static. A
#                                           consumer lists it FIRST in target_link_libraries, so the driver archive
#                                           resolves the C client symbols before aeron_static is scanned (a static
#                                           library here would let the link order put aeron_static first).
#                                           Link it only from final executables, first in the link list, never
#                                           from a library: a library would carry the driver object in its archive
#                                           and the link-order guarantee would no longer hold in each binary.
#
# Headers install under include/cluster_tools/runtime/; the namespace is cluster_tools::runtime.

add_library(ct_runtime STATIC src/HttpServer.cpp src/Supervisor.cpp)
add_library(cluster_tools::runtime ALIAS ct_runtime)
set_target_properties(ct_runtime PROPERTIES EXPORT_NAME runtime)
target_include_directories(ct_runtime PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_compile_features(ct_runtime PUBLIC cxx_std_20)
target_link_libraries(ct_runtime PUBLIC Threads::Threads)
target_compile_options(ct_runtime PRIVATE -Wall -Wextra)

# The idle vocabulary (IdleMode.h), the duty policies and the shipped idle overlays against the embedded driver's mapping
if(BUILD_TESTING)
    add_executable(ct_runtime_idle_tests tests/IdleModeTest.cpp)
    target_link_libraries(ct_runtime_idle_tests PRIVATE ct_runtime)
    target_include_directories(ct_runtime_idle_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
    target_compile_definitions(ct_runtime_idle_tests PRIVATE CT_MEDIA_DRIVER_DIR="${PROJECT_SOURCE_DIR}/deploy/media-driver")
    target_compile_options(ct_runtime_idle_tests PRIVATE -Wall -Wextra)
    add_test(NAME runtime.idle_mode COMMAND ct_runtime_idle_tests)
endif()

# The embedded driver's one source file: the build tree's ct-forwarder and ct-bridge compile it into their final targets
# (their link lines name aeron_driver_static first); the installed package carries it as an interface source.
set(CT_EMBEDDED_DRIVER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/EmbeddedDriver.cpp" CACHE INTERNAL "the runtime's embedded driver")

if(WITH_AERON)
    add_library(ct_runtime_aeron STATIC src/ConductorWatch.cpp)
    add_library(cluster_tools::runtime_aeron ALIAS ct_runtime_aeron)
    set_target_properties(ct_runtime_aeron PROPERTIES EXPORT_NAME runtime_aeron)
    target_link_libraries(ct_runtime_aeron PUBLIC ct_runtime ct_client_aeron)
    target_compile_options(ct_runtime_aeron PRIVATE -Wall -Wextra)

    if(CT_BUILD_MEDIA_DRIVER)
        add_library(ct_runtime_embedded_driver INTERFACE)
        add_library(cluster_tools::runtime_embedded_driver ALIAS ct_runtime_embedded_driver)
        set_target_properties(ct_runtime_embedded_driver PROPERTIES EXPORT_NAME runtime_embedded_driver)
        target_sources(ct_runtime_embedded_driver INTERFACE
            "$<BUILD_INTERFACE:${CT_EMBEDDED_DRIVER_SOURCE}>"
            "$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_DATADIR}/cluster_tools/src/EmbeddedDriver.cpp>")
        target_link_libraries(ct_runtime_embedded_driver INTERFACE aeron_driver_static ct_runtime)
    endif()
endif()
