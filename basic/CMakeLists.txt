cmake_minimum_required(VERSION 3.14)
project(berrybasic_tests C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# The interpreter lives one directory up, alongside its host backends.
set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/..)

# --- Catch2 v3 --------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.5.0
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)   # for catch_discover_tests

# --- the unit-test executable ----------------------------------------------
# = the interpreter (basic.c) + the host backends it needs + the in-memory
# console + the Catch2 tests. host/main.c and host/console_host.c are left out:
# the tests provide their own main (Catch2) and console (test_console.c).
add_executable(unit_tests
  test.cpp
  test_console.c
  ${CMAKE_CURRENT_SOURCE_DIR}/basic.c
  ${REPO}/host/storage_host.c
  ${REPO}/seed/seed_host.c
  ${REPO}/host/image_host.c
  ${REPO}/host/sound_host.c
  ${REPO}/host/gpio_host.c
  ${REPO}/host/i2c_host.c
  ${REPO}/host/ttf_host.c
  ${REPO}/host/gfx_host.c
  ${REPO}/drivers/usb_hid.c
)
# Absolute path to a bundled TTF so the font tests don't depend on the CWD.
target_compile_definitions(unit_tests PRIVATE
  FONT_TTF_PATH="${REPO}/fonts/Philosopher-Regular.ttf")
target_include_directories(unit_tests PRIVATE
  ${REPO}/include ${REPO}/basic ${REPO}/seed ${REPO}/drivers ${REPO}/third_party)
target_link_libraries(unit_tests PRIVATE Catch2::Catch2WithMain)

enable_testing()
include(Catch)
catch_discover_tests(unit_tests)

# --- code coverage ----------------------------------------------------------
# cmake -S basic -B build/tests -DCOVERAGE=ON && cmake --build build/tests --target coverage
option(COVERAGE "instrument the interpreter and add the `coverage` target" OFF)
if(COVERAGE)
  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CodeCoverage.cmake)
  # Instrument only the code under test (basic.c), not the harness or Catch2 —
  # that keeps the report focused and avoids mixing gcov data across compilers.
  set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/basic.c PROPERTIES
    COMPILE_OPTIONS "-g;-O0;--coverage")
  target_link_options(unit_tests PRIVATE --coverage)

  setup_coverage_target(
    NAME       coverage
    EXECUTABLE unit_tests
    BASE_DIR   ${REPO}
    EXCLUDE    "*/host/*" "*/seed/*" "*/third_party/*" "*/_deps/*" "/usr/*"
               "*/test.cpp" "*/test_console.c"
  )
endif()
