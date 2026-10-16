cmake_minimum_required(VERSION 3.20)
project(seeded_tokenizer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

add_executable(tokenizer_test tests/tokenizer_test.cpp src/notes.cpp)
target_include_directories(tokenizer_test PRIVATE include)
add_test(NAME tokenizer COMMAND tokenizer_test)
