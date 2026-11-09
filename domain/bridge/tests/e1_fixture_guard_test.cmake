# bridge.e1_fixture_guard: the committed E1 fixtures verify; a scratch copy with one byte edited, a
# regenerated SUMS without a lock bump, an e1.pin naming another capture, a placeholder pin and an unlisted file are
# each refused for the named reason.
#   cmake -DCT_SOURCE_DIR=<repo> -DCT_WORK_DIR=<scratch> -P e1_fixture_guard_test.cmake
set(verify "${CT_SOURCE_DIR}/cmake/VerifyE1Fixtures.cmake")
set(fixtures "${CT_SOURCE_DIR}/domain/bridge/tests/fixtures/e1")
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
    configure_file("${lock}" "${dir}/dependencies.lock" COPYONLY)
    set(${out_dir} "${dir}/fixtures" PARENT_SCOPE)
    set(${out_lock} "${dir}/dependencies.lock" PARENT_SCOPE)
endfunction()

function(expect_refusal name dir lockfile pattern)
    run_verify("${dir}" "${lockfile}" code out)
    if(code EQUAL 0)
        message(FATAL_ERROR "FAIL ${name}: tampered E1 fixtures were accepted")
    endif()
    if(NOT out MATCHES "${pattern}")
        message(FATAL_ERROR "FAIL ${name}: refused for another reason: ${out}")
    endif()
    message(STATUS "PASS ${name}")
endfunction()

run_verify("${fixtures}" "${lock}" code out)
if(NOT code EQUAL 0)
    message(FATAL_ERROR "FAIL the committed E1 fixtures were refused: ${out}")
endif()
message(STATUS "PASS committed E1 fixtures verify")

find_program(PY python3 REQUIRED)
fresh_copy(byte-edit dir lk)
execute_process(COMMAND ${PY} -I -c "import sys; p=sys.argv[1]; b=bytearray(open(p,'rb').read()); b[20]^=1; open(p,'wb').write(b)"
    "${dir}/plain_fill/member0.fjr" RESULT_VARIABLE py_rc)
if(NOT py_rc EQUAL 0)
    message(FATAL_ERROR "FAIL byte-edit: could not edit the scratch copy")
endif()
expect_refusal(e1FixtureByteEdited "${dir}" "${lk}" "differs from SHA256SUMS: plain_fill/member0.fjr")

fresh_copy(regen dir lk)
execute_process(COMMAND ${PY} -I -c "
import hashlib, sys
d = sys.argv[1]
p = d + '/plain_fill/member0.ffr'
b = bytearray(open(p, 'rb').read()); b[30] ^= 1; open(p, 'wb').write(b)
lines = []
for line in open(d + '/SHA256SUMS'):
    h, name = line.rstrip('\\n').split('  ')
    if name == 'plain_fill/member0.ffr':
        h = hashlib.sha256(bytes(b)).hexdigest()
    lines.append(h + '  ' + name + '\\n')
open(d + '/SHA256SUMS', 'w').write(''.join(lines))
" "${dir}" RESULT_VARIABLE py_rc)
expect_refusal(e1RegeneratedWithoutLockBump "${dir}" "${lk}" "SHA256SUMS has sha256 .* dependencies.lock pins")

fresh_copy(other-capture dir lk)
file(READ "${lk}" text)
string(REGEX REPLACE "\"me_journal_e1_match_ref\": \"[0-9a-f]+\"" "\"me_journal_e1_match_ref\": \"421226c0000000000000000000000000000000ab\"" text "${text}")
file(WRITE "${lk}" "${text}")
expect_refusal(e1PinNamesAnotherCapture "${dir}" "${lk}" "does not name the lock's me_journal_e1_match_ref")

fresh_copy(placeholder dir lk)
file(READ "${lk}" text)
string(REGEX REPLACE "\"me_journal_e1_fixtures_sha256sums_sha256\": \"[0-9a-f]+\"" "\"me_journal_e1_fixtures_sha256sums_sha256\": \"PENDING\"" text "${text}")
file(WRITE "${lk}" "${text}")
expect_refusal(e1PlaceholderPin "${dir}" "${lk}" "is not a sha256")

fresh_copy(extra dir lk)
file(WRITE "${dir}/plain_fill/member9.fjr" "x")
expect_refusal(e1UnlistedFile "${dir}" "${lk}" "holds files SHA256SUMS does not name")

file(REMOVE_RECURSE "${CT_WORK_DIR}")
message(STATUS "E1 FIXTURE GUARD PASS")
