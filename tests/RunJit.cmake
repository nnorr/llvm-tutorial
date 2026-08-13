# Runs `toy` over a .ks file in JIT mode and checks that EXPECT appears among
# the evaluated results. Invoked via `cmake -P` from add_test(); a plain
# COMMAND cannot do stdin redirection or output matching on its own.
#
# Required: -DTOY=<binary> -DINPUT=<file.ks> -DEXPECT=<number>

execute_process(
  COMMAND ${TOY}
  INPUT_FILE ${INPUT}
  OUTPUT_VARIABLE Out
  ERROR_VARIABLE Err
  RESULT_VARIABLE Res
)

if(NOT Res EQUAL 0)
  message(FATAL_ERROR "toy exited with ${Res}\n--- stderr ---\n${Err}")
endif()

# Results are printed to stderr as "Evaluated to <n>.000000".
if(NOT Err MATCHES "Evaluated to ${EXPECT}\\.0+")
  message(FATAL_ERROR
    "expected 'Evaluated to ${EXPECT}.000000' in output\n--- stderr ---\n${Err}")
endif()

message(STATUS "ok: ${INPUT} evaluated to ${EXPECT}")
