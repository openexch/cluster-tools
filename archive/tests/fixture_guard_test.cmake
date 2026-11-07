# archive.fixture_guard (RC2 S9a contract 9.2 FixtureGuard and 9.5): the real fixtures verify; a
# scratch copy with one byte edited, a regenerated SUMS without a lock bump, a placeholder source
# commit and an extra file are each refused for the named reason.
#   cmake -DCT_SOURCE_DIR=<repo> -DCT_WORK_DIR=<scratch> -P fixture_guard_test.cmake
set(verify "${CT_SOURCE_DIR}/cmake/VerifyMeJournalFixtures.cmake")
set(fixtures "${CT_SOURCE_DIR}/archive/tests/fixtures/mejournal")
set(lock "${CT_SOURCE_DIR}/dependencies.lock")

function(run_verify dir lockfile result output)
    execute_process(COMMAND ${CMAKE_COMMAND} -DCT_FIXTURE_DIR=${dir} -DCT_LOCK_FILE=${lockfile} -P ${verify}
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    set(${result} "${code}" PARENT_SCOPE)
    string(REGEX REPLACE "[ \t\r\n]+" " " flat "${out}${err}")
    set(${output} "${flat}" PARENT_SCOPE)
endfunction()

function(fresh_copy name out_dir out_lock)
    set(dir "${CT_WORK_DIR}/${name}")
    file(REMOVE_RECURSE "${dir}")
    file(MAKE_DIRECTORY "${dir}")
    file(COPY "${fixtures}/" DESTINATION "${dir}/fixtures")
    configure_file("${lock}" "${dir}/dependencies.lock" COPYONLY) # file(COPY_FILE) needs 3.21; the fast job floor is 3.16
    set(${out_dir} "${dir}/fixtures" PARENT_SCOPE)
    set(${out_lock} "${dir}/dependencies.lock" PARENT_SCOPE)
endfunction()

function(expect_refusal name dir lockfile pattern)
    run_verify("${dir}" "${lockfile}" code out)
    if(code EQUAL 0)
        message(FATAL_ERROR "FAIL ${name}: tampered fixtures were accepted")
    endif()
    if(NOT out MATCHES "${pattern}")
        message(FATAL_ERROR "FAIL ${name}: refused for another reason: ${out}")
    endif()
    message(STATUS "PASS ${name}")
endfunction()

run_verify("${fixtures}" "${lock}" code out)
if(NOT code EQUAL 0)
    message(FATAL_ERROR "FAIL the committed fixtures were refused: ${out}")
endif()
message(STATUS "PASS committed fixtures verify")

# One byte edited in a fixture (RED of 9.2): byte 16 (meEpoch low byte) flipped. cmake cannot write
# arbitrary bytes, so the edit is done by python (the gate scripts use it too).
find_program(PY python3 REQUIRED)
fresh_copy(byte-edit dir lk)
execute_process(COMMAND ${PY} -I -c "import sys; p=sys.argv[1]; b=bytearray(open(p,'rb').read()); b[16]^=1; open(p,'wb').write(b)"
    "${dir}/java-trade.bin" RESULT_VARIABLE py_rc)
if(NOT py_rc EQUAL 0)
    message(FATAL_ERROR "FAIL byte-edit: could not edit the scratch copy")
endif()
expect_refusal(fixtureByteEdited "${dir}" "${lk}" "differs from SHA256SUMS: java-trade.bin")

# A regenerated dump: fixture and SUMS line both changed, lock not bumped.
fresh_copy(regen dir lk)
execute_process(COMMAND ${PY} -I -c "
import hashlib, sys
d = sys.argv[1]
p = d + '/java-config.bin'
b = bytearray(open(p, 'rb').read()); b[30] ^= 1; open(p, 'wb').write(b)
lines = []
for line in open(d + '/SHA256SUMS'):
    h, name = line.rstrip('\\n').split('  ')
    if name == 'java-config.bin':
        h = hashlib.sha256(bytes(b)).hexdigest()
    lines.append(h + '  ' + name + '\\n')
open(d + '/SHA256SUMS', 'w').write(''.join(lines))
" "${dir}" RESULT_VARIABLE py_rc)
expect_refusal(regeneratedDumpWithoutLockBump "${dir}" "${lk}" "SHA256SUMS has sha256 .* dependencies.lock pins")

# A placeholder source commit in the lock.
fresh_copy(commit dir lk)
file(READ "${lk}" text)
string(REGEX REPLACE "\"me_journal_fixture_source_commit\": \"[0-9a-f]+\"" "\"me_journal_fixture_source_commit\": \"PENDING-match\"" text "${text}")
file(WRITE "${lk}" "${text}")
expect_refusal(placeholderSourceCommit "${dir}" "${lk}" "is not a full match commit sha")

# An extra file next to the six.
fresh_copy(extra dir lk)
file(WRITE "${dir}/java-extra.bin" "x")
expect_refusal(unlistedFixtureFile "${dir}" "${lk}" "holds files SHA256SUMS does not name")

file(REMOVE_RECURSE "${CT_WORK_DIR}")
message(STATUS "FIXTURE GUARD PASS")
