# Runs the matcher on ${CASE}.in and compares stdout, stderr and the exit code
# byte-for-byte with ${CASE}.out, ${CASE}.err and ${CASE}.code (default 0).
# @spec DLV-TEST-001
execute_process(
  COMMAND ${MATCHER}
  INPUT_FILE ${CASE}.in
  OUTPUT_VARIABLE actual_out
  ERROR_VARIABLE actual_err
  RESULT_VARIABLE actual_code)

file(READ ${CASE}.out expected_out)
set(expected_err "")
if(EXISTS ${CASE}.err)
  file(READ ${CASE}.err expected_err)
endif()
set(expected_code 0)
if(EXISTS ${CASE}.code)
  file(STRINGS ${CASE}.code expected_code LIMIT_COUNT 1)
endif()

set(failed FALSE)
if(NOT actual_out STREQUAL expected_out)
  message("stdout mismatch for ${CASE}\n--- expected ---\n${expected_out}--- actual ---\n${actual_out}")
  set(failed TRUE)
endif()
if(NOT actual_err STREQUAL expected_err)
  message("stderr mismatch for ${CASE}\n--- expected ---\n${expected_err}--- actual ---\n${actual_err}")
  set(failed TRUE)
endif()
if(NOT actual_code STREQUAL expected_code)
  message("exit code mismatch for ${CASE}: expected ${expected_code}, got ${actual_code}")
  set(failed TRUE)
endif()
if(failed)
  message(FATAL_ERROR "golden test failed: ${CASE}")
endif()
