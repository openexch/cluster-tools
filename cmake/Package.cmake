# The installed package: `cmake --install <build> --prefix <dir>` writes the shared cluster client and the process
# machinery for another C++20 repository, which then does
#
#     find_package(cluster_tools CONFIG REQUIRED COMPONENTS client_core runtime client_aeron runtime_aeron)
#
# and links the cluster_tools:: targets. Nothing is copied into the consumer: it pins this repository by commit sha and
# source tarball sha256 and builds the package from it (the same verified Aeron tarball, SBE jars and wire schemas as
# any other build of this tree; scripts/fetch-deps.sh, scripts/fetch-schemas.sh).
#
# Components (each a target cluster_tools::<name>; a requested component the build did not make fails find_package
# and names it):
#   every configuration   client_core    endpoints, credentials, backoff, client settings, protocol vocabulary
#                         runtime        Supervisor, HttpServer, LogRing, StatusBoardT, IdleStrategy, DriverTimeout,
#                                        DirProbe, StorageStarvation
#   WITH_AERON            client         the ClusterClient state machine and the core session registrar (header-only)
#                         client_aeron   the Aeron transport and the fatal-error policy
#                         runtime_aeron  ConductorWatch
#                         sbe_codecs     the generated cluster-protocol and money-schema codecs this build compiled,
#                                        and cluster_tools/build_info.h (the schema version and sha256 they came from)
#                         aeron_static, aeron_client_wrapper   the Aeron C client and its C++ wrapper, from the
#                                        verified tarball (the consumer links Aeron through the package, never its own)
#   WITH_AERON and CT_BUILD_MEDIA_DRIVER
#                         runtime_embedded_driver, aeron_driver_static   the embedded media driver (list it first)
#
# The config file also sets cluster_tools_SOURCE_SHA (the commit this package was built from, as /health.build),
# cluster_tools_WITH_AERON and, WITH_AERON, cluster_tools_MONEY_SCHEMA_VERSION / _SHA256: a consumer checks them
# against its own lock and refuses a package built from another commit or schema.
#
# Never installed: the binaries (ct-forwarder, ct-bridge, switch-behavior, ct-journal-tail), the test builds and the
# session stub. The binaries ship through publish.yml.

include(CMakePackageConfigHelpers)

if(CT_SOURCE_SHA_EFFECTIVE STREQUAL "unknown")
    message(WARNING "the package's source sha is \"unknown\" (no git checkout and no CT_BUILD_SOURCE_SHA): a consumer's "
        "pin check will refuse it. Pass -DCT_BUILD_SOURCE_SHA=<40-hex sha> when building from a source tarball.")
endif()

set(CT_PACKAGE_TARGETS ct_client_core ct_runtime)
set(CT_PACKAGE_COMPONENTS client_core runtime)
if(WITH_AERON)
    list(APPEND CT_PACKAGE_TARGETS ct_sbe_codecs ct_client ct_client_aeron ct_runtime_aeron aeron_static aeron_client_wrapper)
    list(APPEND CT_PACKAGE_COMPONENTS sbe_codecs client client_aeron runtime_aeron aeron_static aeron_client_wrapper)
    # Aeron's own targets carry $<INSTALL_INTERFACE:include/aeron> and include/wrapper; the headers go there.
    set_target_properties(aeron_static PROPERTIES EXPORT_NAME aeron_static)
    set_target_properties(aeron_client_wrapper PROPERTIES EXPORT_NAME aeron_client_wrapper)
    if(CT_BUILD_MEDIA_DRIVER)
        list(APPEND CT_PACKAGE_TARGETS ct_runtime_embedded_driver aeron_driver_static)
        list(APPEND CT_PACKAGE_COMPONENTS runtime_embedded_driver aeron_driver_static)
        set_target_properties(aeron_driver_static PROPERTIES EXPORT_NAME aeron_driver_static)
    endif()
endif()

set(CT_PACKAGE_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/cluster_tools")

install(TARGETS ${CT_PACKAGE_TARGETS} EXPORT cluster_tools_targets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/cluster_tools"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/cluster_tools"
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

install(DIRECTORY "${PROJECT_SOURCE_DIR}/client/include/" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    FILES_MATCHING PATTERN "*.h")
install(DIRECTORY "${PROJECT_SOURCE_DIR}/runtime/include/" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    FILES_MATCHING PATTERN "*.h")

if(WITH_AERON)
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/client/aeron/include/" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        FILES_MATCHING PATTERN "*.h")
    install(DIRECTORY "${CT_SBE_GEN_DIR}/cluster/" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/cluster_tools/sbe/cluster"
        FILES_MATCHING PATTERN "*.h")
    install(DIRECTORY "${CT_SBE_GEN_DIR}/money/" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/cluster_tools/sbe/money"
        FILES_MATCHING PATTERN "*.h")
    install(DIRECTORY "${CT_SBE_GEN_DIR}/info/" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/cluster_tools/sbe/info"
        FILES_MATCHING PATTERN "*.h")
    install(DIRECTORY "${AERON_SRC}/aeron-client/src/main/c/" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/aeron"
        FILES_MATCHING PATTERN "*.h")
    install(DIRECTORY "${AERON_SRC}/aeron-client/src/main/cpp_wrapper/" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/wrapper"
        FILES_MATCHING PATTERN "*.h")
    if(CT_BUILD_MEDIA_DRIVER)
        install(DIRECTORY "${AERON_SRC}/aeron-driver/src/main/c/" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/aeronmd"
            FILES_MATCHING PATTERN "*.h")
        install(FILES "${CT_EMBEDDED_DRIVER_SOURCE}" DESTINATION "${CMAKE_INSTALL_DATADIR}/cluster_tools/src")
    endif()
    set(CT_PACKAGE_MONEY_SCHEMA_VERSION "${CT_MONEY_VERSION}")
    set(CT_PACKAGE_MONEY_SCHEMA_SHA256 "${CT_MONEY_SHA256}")
    set(CT_PACKAGE_AERON_VERSION "${CT_AERON_VERSION}")
    set(CT_PACKAGE_WITH_AERON TRUE)
else()
    set(CT_PACKAGE_MONEY_SCHEMA_VERSION "")
    set(CT_PACKAGE_MONEY_SCHEMA_SHA256 "")
    set(CT_PACKAGE_AERON_VERSION "")
    set(CT_PACKAGE_WITH_AERON FALSE)
endif()

install(EXPORT cluster_tools_targets NAMESPACE cluster_tools:: FILE cluster_toolsTargets.cmake
    DESTINATION "${CT_PACKAGE_CMAKE_DIR}")

configure_package_config_file("${PROJECT_SOURCE_DIR}/cmake/cluster_toolsConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/package/cluster_toolsConfig.cmake"
    INSTALL_DESTINATION "${CT_PACKAGE_CMAKE_DIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/package/cluster_toolsConfig.cmake" DESTINATION "${CT_PACKAGE_CMAKE_DIR}")
