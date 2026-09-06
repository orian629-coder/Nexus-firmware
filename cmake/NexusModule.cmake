# Helpers for declaring Nexus modules as independent static libraries, each with its own
# optional unit-test target. Keeping one library per module makes every module compilable and
# testable in isolation and makes the dependency graph (and thus layering violations) explicit.

# nexus_add_module(<name>
#   SOURCES <a.cpp> ...
#   DEPS    <target> ...)   # libraries this module links (e.g. nexus_core, nexus::sodium)
#
# Creates target `nexus_<name>`. Every module can include headers relative to the src/ root
# (e.g. #include "core/IService.h").
function(nexus_add_module name)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})

  set(target "nexus_${name}")
  add_library(${target} STATIC ${ARG_SOURCES})
  target_include_directories(${target} PUBLIC ${NEXUS_SRC_DIR})
  target_link_libraries(${target} PUBLIC ${ARG_DEPS})
  nexus_apply_warnings(${target})
  set_target_properties(${target} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endfunction()

# nexus_add_test(<module>
#   SOURCES <x_test.cpp> ...
#   DEPS    <target> ...)   # extra libs beyond the module + gtest
#
# Creates test executable `nexus_<module>_test` linked against `nexus_<module>`, GTest, and
# registers it with CTest. No-op when NEXUS_BUILD_TESTS is off.
function(nexus_add_test module)
  if(NOT NEXUS_BUILD_TESTS)
    return()
  endif()
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})

  set(target "nexus_${module}_test")
  add_executable(${target} ${ARG_SOURCES})
  target_link_libraries(${target} PRIVATE nexus_${module} GTest::gtest GTest::gtest_main ${ARG_DEPS})
  set_target_properties(${target} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  gtest_discover_tests(${target})
endfunction()
