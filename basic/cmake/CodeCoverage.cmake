# ---------------------------------------------------------------------------
# CodeCoverage.cmake  —  minimal, modern gcov coverage helper
#
# Provides:  setup_coverage_target(NAME <target>
#                                   EXECUTABLE <test-target>
#                                   [BASE_DIR <dir>]
#                                   [OUTPUT <dir>]
#                                   [EXCLUDE <glob>...])
#
# Building the resulting target runs the tests and produces:
#   * an HTML report in <OUTPUT>/index.html
#   * a per-file summary printed to the terminal
#
# It prefers gcovr (a single, actively-maintained tool) when available and
# otherwise falls back to lcov + genhtml. Either way it uses the gcov that
# matches the compiler, so gcc/gcov version drift doesn't break the report.
# ---------------------------------------------------------------------------

# Pick the gcov matching the C compiler (e.g. gcc 11 -> gcov-11); the bare
# `gcov` on PATH may belong to an unrelated toolchain.
if(CMAKE_C_COMPILER_VERSION)
  string(REGEX MATCH "^[0-9]+" _cc_major "${CMAKE_C_COMPILER_VERSION}")
endif()
find_program(GCOV_TOOL NAMES gcov-${_cc_major} gcov)
find_program(GCOVR_PATH gcovr)
find_program(LCOV_PATH lcov)
find_program(GENHTML_PATH genhtml)

if(NOT GCOV_TOOL)
  message(FATAL_ERROR "CodeCoverage: no gcov found")
endif()
if(NOT GCOVR_PATH AND NOT (LCOV_PATH AND GENHTML_PATH))
  message(FATAL_ERROR "CodeCoverage: need either gcovr, or lcov + genhtml")
endif()

function(setup_coverage_target)
  cmake_parse_arguments(COV "" "NAME;EXECUTABLE;BASE_DIR;OUTPUT" "EXCLUDE" ${ARGN})
  if(NOT COV_NAME OR NOT COV_EXECUTABLE)
    message(FATAL_ERROR "setup_coverage_target: NAME and EXECUTABLE are required")
  endif()
  if(NOT COV_BASE_DIR)
    set(COV_BASE_DIR ${CMAKE_SOURCE_DIR})
  endif()
  if(NOT COV_OUTPUT)
    set(COV_OUTPUT ${CMAKE_BINARY_DIR}/coverage)
  endif()

  # Run the tests but never let a failing test abort report generation.
  set(_run sh -c "$<TARGET_FILE:${COV_EXECUTABLE}> || true")

  if(GCOVR_PATH)
    # --- modern single-tool path (gcovr) ------------------------------------
    set(_excl "")
    foreach(p ${COV_EXCLUDE})
      list(APPEND _excl --exclude "${p}")
    endforeach()
    add_custom_target(${COV_NAME}
      COMMAND ${_run}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${COV_OUTPUT}
      COMMAND ${GCOVR_PATH} --gcov-executable ${GCOV_TOOL} --root ${COV_BASE_DIR}
              ${_excl} --html-details ${COV_OUTPUT}/index.html --print-summary
              ${CMAKE_BINARY_DIR}
      DEPENDS ${COV_EXECUTABLE}
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
      COMMENT "Running tests + gcovr coverage -> ${COV_OUTPUT}/index.html"
      VERBATIM)
  else()
    # --- fallback path (lcov + genhtml) -------------------------------------
    # --ignore-errors keeps lcov 1.x from bailing on minor gcov quirks.
    set(_lcov ${LCOV_PATH} --gcov-tool ${GCOV_TOOL} --ignore-errors gcov,source,graph)
    set(_info ${CMAKE_BINARY_DIR}/coverage.info)
    set(_rm "")
    foreach(p ${COV_EXCLUDE})
      list(APPEND _rm "${p}")
    endforeach()
    add_custom_target(${COV_NAME}
      COMMAND ${_lcov} --directory . --zerocounters -q
      COMMAND ${_run}
      COMMAND ${_lcov} --directory . --base-directory ${COV_BASE_DIR}
              --capture --output-file ${_info} -q
      COMMAND ${_lcov} --remove ${_info} ${_rm} --output-file ${_info} -q
      COMMAND ${GENHTML_PATH} ${_info} --output-directory ${COV_OUTPUT} -q
      COMMAND ${_lcov} --list ${_info}
      DEPENDS ${COV_EXECUTABLE}
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
      COMMENT "Running tests + lcov coverage -> ${COV_OUTPUT}/index.html"
      VERBATIM)
  endif()

  add_custom_command(TARGET ${COV_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "Coverage report: ${COV_OUTPUT}/index.html")
endfunction()
