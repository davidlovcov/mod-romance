# mod-romance standalone tests: builds the engine-independent core (src/core)
# without AzerothCore and runs unit + integration tests with mocked Ollama.
#
#   cmake -S tests -B build-tests && cmake --build build-tests -j && ctest --test-dir build-tests --output-on-failure

cmake_minimum_required(VERSION 3.16)
project(mod_romance_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CORE_DIR ${CMAKE_CURRENT_LIST_DIR}/../src/core)
file(GLOB CORE_SOURCES ${CORE_DIR}/*.cpp)

add_library(romance_core STATIC ${CORE_SOURCES})
target_include_directories(romance_core PUBLIC ${CORE_DIR})
find_package(Threads REQUIRED)
target_link_libraries(romance_core PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(romance_core PUBLIC ws2_32)
endif()
if(MSVC)
  target_compile_options(romance_core PRIVATE /W4)
else()
  target_compile_options(romance_core PRIVATE -Wall -Wextra)
endif()

option(ROMANCE_SANITIZE "Build tests with address/undefined/thread sanitizers (ASAN/UBSAN)" OFF)
if(ROMANCE_SANITIZE AND NOT MSVC)
  target_compile_options(romance_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(romance_core PUBLIC -fsanitize=address,undefined)
endif()

set(TEST_SOURCES test_main.cpp test_core_units.cpp test_service.cpp test_commands.cpp)
if(NOT WIN32)
  list(APPEND TEST_SOURCES test_http.cpp)
endif()
add_executable(romance_tests ${TEST_SOURCES})
target_include_directories(romance_tests PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/mock)
target_link_libraries(romance_tests PRIVATE romance_core)

enable_testing()
add_test(NAME romance_tests COMMAND romance_tests)

# C++ side of the wire-format interop test (tests/test_interop.lua in azerothcore-romance-addon)
add_executable(romance_wire wire_tool.cpp)
target_link_libraries(romance_wire PRIVATE romance_core)
