# Golden T2 seed: the agents may only write under include/ (coder) and tests/
# (QA), so the build skeleton comes from the harness. One test executable per
# tests/*.cpp keeps independent agent-authored files with their own main()
# from colliding.
cmake_minimum_required(VERSION 3.20)
project(golden_registry CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
foreach(source IN LISTS TEST_SOURCES)
  get_filename_component(name "${source}" NAME_WE)
  add_executable("${name}" "${source}")
  target_include_directories("${name}" PRIVATE include)
  add_test(NAME "${name}" COMMAND "${name}")
endforeach()
