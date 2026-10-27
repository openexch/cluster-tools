# The verified-dependency gate for the WITH_CODECS / WITH_AERON builds (pattern:
# oms/execution-projector). Nothing is trusted by path or by name: Aeron (WITH_AERON only) is
# built only from the source TARBALL whose sha256 dependencies.lock pins (configure hashes it
# and extracts it itself), the SBE jars are hashed and then executed from hashed copies inside
# the build tree, and every wire schema must have the schema id, version floor and templates
# the lock names. A failure here stops configure; it never degrades to "use what is there".
# WITH_CODECS needs Java only (codegen for money, order and ME-journal schemas); WITH_AERON
# adds the Aeron tree, the cluster-protocol codecs and everything that links Aeron.

if(WITH_AERON AND CMAKE_VERSION VERSION_LESS 3.30)
    message(FATAL_ERROR "WITH_AERON=ON needs cmake >= 3.30 (Aeron 1.53.1's floor), have ${CMAKE_VERSION}")
endif()
if(CMAKE_VERSION VERSION_LESS 3.21)
    message(FATAL_ERROR "WITH_CODECS=ON needs cmake >= 3.21 (file(COPY_FILE)), have ${CMAKE_VERSION}")
endif()

string(JSON CT_LOCK_AERON_VERSION GET "${CT_DEPENDENCY_LOCK}" aeron_version)
string(JSON CT_LOCK_AERON_SHA GET "${CT_DEPENDENCY_LOCK}" aeron_source_sha256)
string(JSON CT_LOCK_SBE_VERSION GET "${CT_DEPENDENCY_LOCK}" sbe_version)
string(JSON CT_LOCK_SBE_SHA GET "${CT_DEPENDENCY_LOCK}" sbe_sha256)
string(JSON CT_LOCK_AGRONA_SHA GET "${CT_DEPENDENCY_LOCK}" agrona_sha256)

# ---- Aeron: the tarball is the input, never a tree.
set(CT_AERON_VERSION "")
if(WITH_AERON)
if(DEFINED AERON_SRC AND NOT AERON_SRC STREQUAL "")
    message(FATAL_ERROR "AERON_SRC is not an input any more: a source tree cannot be hash-verified. "
        "Pass -DAERON_TARBALL=<aeron-${CT_LOCK_AERON_VERSION}.tar.gz> (scripts/fetch-deps.sh).")
endif()
if(NOT EXISTS "${AERON_TARBALL}")
    message(FATAL_ERROR "WITH_AERON=ON needs -DAERON_TARBALL=<aeron-${CT_LOCK_AERON_VERSION}.tar.gz> "
        "(scripts/fetch-deps.sh); got '${AERON_TARBALL}'")
endif()
file(SHA256 "${AERON_TARBALL}" CT_AERON_TARBALL_SHA)
if(NOT CT_AERON_TARBALL_SHA STREQUAL CT_LOCK_AERON_SHA)
    message(FATAL_ERROR "AERON_TARBALL ${AERON_TARBALL} has sha256 ${CT_AERON_TARBALL_SHA}; "
        "dependencies.lock pins ${CT_LOCK_AERON_SHA}")
endif()
# Extracted by configure into the build tree. Re-extracted whenever the stamp does not name
# the verified hash (first configure, or a different tarball); kept otherwise so a reconfigure
# does not rebuild Aeron.
set(CT_AERON_EXTRACT_DIR "${CMAKE_BINARY_DIR}/_deps/aeron")
set(CT_AERON_STAMP "${CT_AERON_EXTRACT_DIR}.sha256")
set(CT_AERON_STAMP_CONTENT "")
if(EXISTS "${CT_AERON_STAMP}")
    file(READ "${CT_AERON_STAMP}" CT_AERON_STAMP_CONTENT)
endif()
if(NOT CT_AERON_STAMP_CONTENT STREQUAL CT_AERON_TARBALL_SHA OR NOT EXISTS "${CT_AERON_EXTRACT_DIR}")
    file(REMOVE_RECURSE "${CT_AERON_EXTRACT_DIR}")
    file(REMOVE "${CT_AERON_STAMP}")
    file(ARCHIVE_EXTRACT INPUT "${AERON_TARBALL}" DESTINATION "${CT_AERON_EXTRACT_DIR}")
    file(WRITE "${CT_AERON_STAMP}" "${CT_AERON_TARBALL_SHA}")
endif()
file(GLOB CT_AERON_TOP LIST_DIRECTORIES true "${CT_AERON_EXTRACT_DIR}/*")
list(LENGTH CT_AERON_TOP CT_AERON_TOP_COUNT)
if(NOT CT_AERON_TOP_COUNT EQUAL 1 OR NOT IS_DIRECTORY "${CT_AERON_TOP}")
    message(FATAL_ERROR "AERON_TARBALL must hold exactly one top-level directory, found: ${CT_AERON_TOP}")
endif()
set(AERON_SRC "${CT_AERON_TOP}")
file(STRINGS "${AERON_SRC}/version.txt" CT_AERON_VERSION LIMIT_COUNT 1)
if(NOT CT_AERON_VERSION STREQUAL CT_LOCK_AERON_VERSION)
    message(FATAL_ERROR "AERON_TARBALL holds Aeron ${CT_AERON_VERSION}; dependencies.lock pins ${CT_LOCK_AERON_VERSION}")
endif()
message(STATUS "Aeron ${CT_AERON_VERSION} from ${AERON_TARBALL} (sha256 ${CT_AERON_TARBALL_SHA}), extracted to ${AERON_SRC}")
endif()

# ---- SBE classpath: exactly the two pinned jars, each verified, then COPIED into the build
# tree; codegen runs only from the copies and gen-codecs.sh re-hashes them before every run, so
# a jar replaced after configure is never executed.
if(SBE_CLASSPATH STREQUAL "")
    message(FATAL_ERROR "WITH_AERON=ON needs -DSBE_CLASSPATH=<sbe-tool.jar>:<agrona.jar> (scripts/fetch-deps.sh)")
endif()
string(REPLACE ":" ";" CT_SBE_JARS "${SBE_CLASSPATH}")
set(CT_SBE_COPY_DIR "${CMAKE_BINARY_DIR}/_deps/sbe")
set(CT_SBE_SEEN FALSE)
set(CT_AGRONA_SEEN FALSE)
set(CT_SBE_VERIFIED_JARS)
foreach(jar IN LISTS CT_SBE_JARS)
    if(NOT EXISTS "${jar}")
        message(FATAL_ERROR "SBE_CLASSPATH entry not found: ${jar}")
    endif()
    file(SHA256 "${jar}" jar_sha)
    if(jar_sha STREQUAL CT_LOCK_SBE_SHA)
        set(CT_SBE_SEEN TRUE)
        set(copy "${CT_SBE_COPY_DIR}/sbe-tool-${CT_LOCK_SBE_VERSION}.jar")
    elseif(jar_sha STREQUAL CT_LOCK_AGRONA_SHA)
        set(CT_AGRONA_SEEN TRUE)
        set(copy "${CT_SBE_COPY_DIR}/agrona.jar")
    else()
        message(FATAL_ERROR "SBE_CLASSPATH entry ${jar} (sha256 ${jar_sha}) is not a jar dependencies.lock pins")
    endif()
    file(MAKE_DIRECTORY "${CT_SBE_COPY_DIR}")
    file(COPY_FILE "${jar}" "${copy}" ONLY_IF_DIFFERENT)
    file(SHA256 "${copy}" copy_sha)
    if(NOT copy_sha STREQUAL jar_sha)
        message(FATAL_ERROR "copy of ${jar} does not hash like the original")
    endif()
    list(APPEND CT_SBE_VERIFIED_JARS "${copy}")
endforeach()
if(NOT CT_SBE_SEEN OR NOT CT_AGRONA_SEEN)
    message(FATAL_ERROR "SBE_CLASSPATH must hold both the pinned sbe-tool and agrona jars")
endif()
string(REPLACE ";" ":" CT_SBE_VERIFIED_CLASSPATH "${CT_SBE_VERIFIED_JARS}")
set(CT_SBE_JAR_SHA256S "${CT_LOCK_SBE_SHA} ${CT_LOCK_AGRONA_SHA}")

# ct_check_schema(<label> <path> <lock-prefix> <out-var-prefix>)
# Structural check, not a hash pin: a newer schema (a later money or order version) passes by file swap
# as long as it keeps the id, does not go below the floor and still defines every template
# the client code is compiled against. The hash and version are recorded in build_info.h.
function(ct_check_schema label path prefix out)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "${label}: -D${label}=<schema.xml> is required (scripts/fetch-schemas.sh); got '${path}'")
    endif()
    file(READ "${path}" xml)
    string(REGEX MATCH "<sbe:messageSchema[^>]*>" root "${xml}")
    string(REGEX MATCH "[ \t\r\n]id=\"([0-9]+)\"" _ "${root}")
    set(id "${CMAKE_MATCH_1}")
    string(REGEX MATCH "[ \t\r\n]version=\"([0-9]+)\"" _ "${root}")
    set(version "${CMAKE_MATCH_1}")
    string(JSON want_id GET "${CT_DEPENDENCY_LOCK}" ${prefix}_schema_id)
    string(JSON min_version GET "${CT_DEPENDENCY_LOCK}" ${prefix}_schema_min_version)
    if(NOT id STREQUAL want_id)
        message(FATAL_ERROR "${label} ${path}: schema id '${id}', the client speaks id ${want_id}")
    endif()
    if(version STREQUAL "" OR version LESS min_version)
        message(FATAL_ERROR "${label} ${path}: schema version '${version}' is below the floor ${min_version} (dependencies.lock)")
    endif()
    string(JSON n LENGTH "${CT_DEPENDENCY_LOCK}" ${prefix}_schema_required_templates)
    math(EXPR last "${n} - 1")
    foreach(i RANGE ${last})
        string(JSON tid GET "${CT_DEPENDENCY_LOCK}" ${prefix}_schema_required_templates ${i})
        if(NOT xml MATCHES "<sbe:message[ \t\r\n]+name=\"[A-Za-z0-9_]+\"[ \t\r\n]+id=\"${tid}\"")
            message(FATAL_ERROR "${label} ${path}: template ${tid} is missing (required by dependencies.lock)")
        endif()
    endforeach()
    file(SHA256 "${path}" sha)
    set(${out}_VERSION "${version}" PARENT_SCOPE)
    set(${out}_SHA256 "${sha}" PARENT_SCOPE)
    message(STATUS "${label}: schema id ${id} version ${version} sha256 ${sha}")
endfunction()

ct_check_schema(MONEY_SCHEMA "${MONEY_SCHEMA}" money CT_MONEY)
ct_check_schema(MATCH_SCHEMA "${MATCH_SCHEMA}" match CT_MATCH)
ct_check_schema(ME_JOURNAL_SCHEMA "${ME_JOURNAL_SCHEMA}" me_journal CT_JOURNAL)
# The ME journal is a fixed on-disk contract (schemaId 4 v1), not a moving wire: its bytes are
# pinned exactly, unlike the two engine wires above which pass on structure.
string(JSON CT_LOCK_JOURNAL_SHA GET "${CT_DEPENDENCY_LOCK}" me_journal_schema_sha256)
if(NOT CT_JOURNAL_SHA256 STREQUAL CT_LOCK_JOURNAL_SHA)
    message(FATAL_ERROR "ME_JOURNAL_SCHEMA ${ME_JOURNAL_SCHEMA} has sha256 ${CT_JOURNAL_SHA256}; "
        "dependencies.lock pins ${CT_LOCK_JOURNAL_SHA}")
endif()

# RC2 S9a (contract 2.2): the bridge reads the ME journal and writes the core money wire, and the
# core gate is exact (v7 only). Under WITH_BRIDGE both schemas are pinned by EXACT sha256 and exact
# root version, unlike the structural floor above: a money v8 file would pass a floor and be refused
# by the exact-7 core gate at run time (R-B1-1's C++ twin).
if(WITH_BRIDGE)
    string(JSON CT_LOCK_MONEY_SHA GET "${CT_DEPENDENCY_LOCK}" money_schema_sha256)
    if(NOT CT_MONEY_SHA256 STREQUAL CT_LOCK_MONEY_SHA)
        message(FATAL_ERROR "WITH_BRIDGE=ON: MONEY_SCHEMA ${MONEY_SCHEMA} has sha256 ${CT_MONEY_SHA256}; "
            "dependencies.lock pins ${CT_LOCK_MONEY_SHA} (the bridge needs the exact core wire)")
    endif()
    string(JSON CT_LOCK_MONEY_VERSION GET "${CT_DEPENDENCY_LOCK}" money_schema_min_version)
    string(JSON CT_LOCK_MONEY_ID GET "${CT_DEPENDENCY_LOCK}" money_schema_id)
    if(NOT CT_MONEY_VERSION STREQUAL CT_LOCK_MONEY_VERSION)
        message(FATAL_ERROR "WITH_BRIDGE=ON: MONEY_SCHEMA ${MONEY_SCHEMA} is version ${CT_MONEY_VERSION}; "
            "the bridge speaks exactly id=\"${CT_LOCK_MONEY_ID}\" version=\"${CT_LOCK_MONEY_VERSION}\"")
    endif()
    string(JSON CT_LOCK_JOURNAL_VERSION GET "${CT_DEPENDENCY_LOCK}" me_journal_schema_min_version)
    string(JSON CT_LOCK_JOURNAL_ID GET "${CT_DEPENDENCY_LOCK}" me_journal_schema_id)
    if(NOT CT_JOURNAL_VERSION STREQUAL CT_LOCK_JOURNAL_VERSION)
        message(FATAL_ERROR "WITH_BRIDGE=ON: ME_JOURNAL_SCHEMA ${ME_JOURNAL_SCHEMA} is version ${CT_JOURNAL_VERSION}; "
            "the bridge reads exactly id=\"${CT_LOCK_JOURNAL_ID}\" version=\"${CT_LOCK_JOURNAL_VERSION}\"")
    endif()
endif()

# RC2 S1 refuter X6: a v12 order schema whose DurableOrderCommand (template 9) has no
# timeInForce is not the schema the lock names, whatever its root version says.
file(READ "${MATCH_SCHEMA}" CT_MATCH_XML)
string(REGEX MATCH "<sbe:message[ \t\r\n]+name=\"[A-Za-z0-9_]+\"[ \t\r\n]+id=\"9\"" CT_T9_OPEN "${CT_MATCH_XML}")
string(FIND "${CT_MATCH_XML}" "${CT_T9_OPEN}" CT_T9_START)
string(SUBSTRING "${CT_MATCH_XML}" ${CT_T9_START} -1 CT_T9_TAIL)
string(FIND "${CT_T9_TAIL}" "</sbe:message>" CT_T9_LEN)
string(SUBSTRING "${CT_T9_TAIL}" 0 ${CT_T9_LEN} CT_T9_BODY)
if(NOT CT_T9_BODY MATCHES "name=\"timeInForce\"")
    message(FATAL_ERROR "MATCH_SCHEMA ${MATCH_SCHEMA}: template 9 has no timeInForce field (the v12 DurableOrderCommand)")
endif()

find_package(Java 21 COMPONENTS Runtime REQUIRED)
find_package(Threads REQUIRED)

if(WITH_AERON)

# Aeron from source, client only by default; the driver too when CT_BUILD_MEDIA_DRIVER.
set(BUILD_AERON_DRIVER ${CT_BUILD_MEDIA_DRIVER} CACHE BOOL "" FORCE)
set(BUILD_AERON_ARCHIVE_API OFF CACHE BOOL "" FORCE)
set(AERON_TESTS OFF CACHE BOOL "" FORCE)
set(AERON_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(AERON_SYSTEM_TESTS OFF CACHE BOOL "" FORCE)
set(AERON_SLOW_SYSTEM_TESTS OFF CACHE BOOL "" FORCE)
set(AERON_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(AERON_BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
set(AERON_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
set(AERON_HIDE_DEPRECATION_MESSAGE ON CACHE BOOL "" FORCE)
add_subdirectory("${AERON_SRC}" aeron EXCLUDE_FROM_ALL)
if(CT_BUILD_MEDIA_DRIVER)
    # aeronmd is EXCLUDE_FROM_ALL inside the aeron tree; pull it into the default build.
    add_custom_target(ct_media_driver ALL DEPENDS aeronmd aeron_driver aeron)
endif()
endif()
