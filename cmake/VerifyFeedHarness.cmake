# RC2 S9h 9.2 (review FIX-3): the feed harness jar guard. The S9h harness jar (assets test scope: the Java reference
# translator, serve, story, t1-*) is the only source of the expected bytes the C++ bridge is graded against, so a
# configure that is given one (CT_FEED_HARNESS_JAR) refuses it unless
#   (a) the money schema sha256 the jar carries (META-INF/feed-harness.properties) equals the lock's money_schema_sha256,
#       and the journal schema sha256 equals me_journal_schema_sha256 (one core, one jar);
#   (b) the jar's production tree equals money_schema_ref: main_tree_equals_ref=true and main_tree_ref is
#       feed_harness_main_diff_empty_vs, which must itself be money_schema_ref (the harness commit's src/main diff
#       against the core the lock pins is empty);
#   (c) the jar's sha256 equals feed_harness_sha256.
# The properties are checked before the hash so a tampered or foreign jar names what is wrong with it.
# Also runnable as a script (and from the test):
#   cmake -DCT_FEED_HARNESS_JAR=<jar> -DCT_LOCK_FILE=<dependencies.lock> -P cmake/VerifyFeedHarness.cmake

function(ct_lock_string lock key out)
    string(REGEX MATCH "\"${key}\"[ \t]*:[ \t]*\"([^\"]*)\"" _ "${lock}")
    if("${CMAKE_MATCH_1}" STREQUAL "")
        message(FATAL_ERROR "dependencies.lock: ${key} is missing")
    endif()
    set(${out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

function(ct_jar_property props key out)
    string(REGEX MATCH "(^|\n)${key}=([^\r\n]*)" _ "${props}")
    set(${out} "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

function(ct_verify_feed_harness jar lock_file work_dir)
    if(NOT EXISTS "${jar}")
        message(FATAL_ERROR "CT_FEED_HARNESS_JAR ${jar} does not exist")
    endif()
    file(READ "${lock_file}" lock)
    ct_lock_string("${lock}" money_schema_sha256 want_money)
    ct_lock_string("${lock}" me_journal_schema_sha256 want_journal)
    ct_lock_string("${lock}" money_schema_ref money_ref)
    ct_lock_string("${lock}" feed_harness_main_diff_empty_vs main_vs)
    ct_lock_string("${lock}" feed_harness_sha256 want_jar)
    if(NOT main_vs STREQUAL money_ref)
        message(FATAL_ERROR "dependencies.lock: feed_harness_main_diff_empty_vs (${main_vs}) must be money_schema_ref "
            "(${money_ref}): the harness is accepted only when its src/main equals the pinned core's")
    endif()
    file(REMOVE_RECURSE "${work_dir}")
    file(MAKE_DIRECTORY "${work_dir}")
    file(ARCHIVE_EXTRACT INPUT "${jar}" DESTINATION "${work_dir}" PATTERNS "META-INF/feed-harness.properties")
    if(NOT EXISTS "${work_dir}/META-INF/feed-harness.properties")
        message(FATAL_ERROR "${jar} carries no META-INF/feed-harness.properties: not an S9h feed harness jar")
    endif()
    file(READ "${work_dir}/META-INF/feed-harness.properties" props)
    ct_jar_property("${props}" money_schema_sha256 jar_money)
    ct_jar_property("${props}" me_journal_schema_sha256 jar_journal)
    ct_jar_property("${props}" main_tree_equals_ref jar_equal)
    ct_jar_property("${props}" main_tree_ref jar_ref)
    if(NOT jar_money STREQUAL want_money)
        message(FATAL_ERROR "feed harness ${jar}: it was built against money schema sha256 '${jar_money}'; "
            "dependencies.lock money_schema_sha256 is ${want_money} (S9h 9.2 a: one core, one jar)")
    endif()
    if(NOT jar_journal STREQUAL want_journal)
        message(FATAL_ERROR "feed harness ${jar}: journal schema sha256 '${jar_journal}'; the lock pins ${want_journal}")
    endif()
    string(LENGTH "${jar_ref}" ref_len)
    if(ref_len LESS 7)
        message(FATAL_ERROR "feed harness ${jar}: main_tree_ref '${jar_ref}' is not a commit")
    endif()
    string(SUBSTRING "${money_ref}" 0 ${ref_len} money_ref_prefix)
    if(NOT jar_equal STREQUAL "true" OR NOT jar_ref STREQUAL money_ref_prefix)
        message(FATAL_ERROR "feed harness ${jar}: main_tree_equals_ref=${jar_equal} main_tree_ref=${jar_ref}; "
            "its production code must equal money_schema_ref ${money_ref} (S9h 9.2, review FIX-6)")
    endif()
    file(SHA256 "${jar}" got_jar)
    if(NOT got_jar STREQUAL want_jar)
        message(FATAL_ERROR "feed harness ${jar} has sha256 ${got_jar}; dependencies.lock feed_harness_sha256 is ${want_jar}")
    endif()
    message(STATUS "feed harness ${jar}: money ${jar_money}, main tree = ${money_ref}, sha256 ${got_jar} (verified)")
endfunction()

if(CMAKE_SCRIPT_MODE_FILE AND DEFINED CT_FEED_HARNESS_JAR)
    if(NOT DEFINED CT_LOCK_FILE)
        message(FATAL_ERROR "usage: cmake -DCT_FEED_HARNESS_JAR=<jar> -DCT_LOCK_FILE=<lock> -P VerifyFeedHarness.cmake")
    endif()
    if(NOT DEFINED CT_WORK_DIR)
        set(CT_WORK_DIR "${CMAKE_CURRENT_BINARY_DIR}/ct-feed-harness-guard")
    endif()
    ct_verify_feed_harness("${CT_FEED_HARNESS_JAR}" "${CT_LOCK_FILE}" "${CT_WORK_DIR}")
endif()
