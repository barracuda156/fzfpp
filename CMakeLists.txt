cmake_minimum_required(VERSION 3.14)
project(fzf++ VERSION 0.2.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Note: using bundled dependencies introduces a build-time dependency on git.

option(USE_SYSTEM_UTFCPP "Use system utfcpp library" OFF)

# Compiler warnings and flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Wno-unused-parameter
    )

    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(-O3 -DNDEBUG)
    endif()
endif()

find_package(Threads REQUIRED)

# utfcpp - Header-only UTF-8 library
if(USE_SYSTEM_UTFCPP)
    find_package(utf8cpp CONFIG QUIET)

    # If not found or target not created properly, do it manually
    if(NOT TARGET utf8cpp)
        find_path(UTF8CPP_INCLUDE_DIR
            NAMES utf8.h
            PATHS /opt/local/include /usr/local/include /usr/include
            PATH_SUFFIXES utf8cpp
            REQUIRED
        )

        add_library(utf8cpp INTERFACE)
        target_include_directories(utf8cpp INTERFACE ${UTF8CPP_INCLUDE_DIR})
        message(STATUS "Using system utf8cpp from: ${UTF8CPP_INCLUDE_DIR}")
    endif()
else()
    # Use bundled utfcpp
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/utfcpp/source/utf8.h")
        message(STATUS "utfcpp not found, fetching the source")
        include(FetchContent)
        FetchContent_Declare(
            utfcpp
            GIT_REPOSITORY https://github.com/nemtrif/utfcpp.git
            GIT_TAG v4.0.5
        )
        FetchContent_MakeAvailable(utfcpp)
    else()
        add_library(utf8cpp INTERFACE)
        target_include_directories(utf8cpp INTERFACE external/utfcpp/source)
    endif()
endif()

# Source files
set(SOURCES
    src/main.cpp
    src/options.cpp
    src/keymap.cpp
    src/tokenizer.cpp
    src/ansi.cpp
    src/chunklist.cpp
    src/matcher.cpp
    src/search.cpp
    src/reader.cpp
    src/terminal.cpp
    src/tty.cpp
    src/keyparser.cpp
    src/render.cpp
)

# Main executable
add_executable(fzf ${SOURCES})

target_include_directories(fzf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(fzf PRIVATE
    utf8cpp
    Threads::Threads
)

# Platform-specific libraries
if(APPLE)
    # On < 10.7 you will need legacy-support for getline.
elseif(UNIX)
    # Linux may need additional libraries
    target_link_libraries(fzf PRIVATE rt)
endif()

# Standalone smoke-test binary for src/tty.hpp (raw mode, resize, self-pipe).
# Not installed, not part of the fzf target's dependency graph.
add_executable(tty_smoketest tools/tty_smoketest.cpp src/tty.cpp)
target_include_directories(tty_smoketest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Standalone unit tests for src/keyparser.cpp — no pty/tty needed, just feeds
# byte strings and asserts on the decoded KeyEvents.
add_executable(keyparser_test tools/keyparser_test.cpp src/keyparser.cpp)
target_include_directories(keyparser_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(keyparser_test PRIVATE utf8cpp)

# Unit + randomized stress tests for src/matcher.cpp — extended-search
# parsing, fzf score parity, and the V1/V2 subsequence/position invariants
# (200k iterations per algorithm).
add_executable(matcher_test tools/matcher_test.cpp src/matcher.cpp)
target_include_directories(matcher_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(matcher_test PRIVATE utf8cpp)

# Unit tests for src/options.cpp -- the hand-written fzf option parser:
# every option name, value forms, precedence, and fzf's error messages.
add_executable(options_test tools/options_test.cpp src/options.cpp)
target_include_directories(options_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(options_test PRIVATE utf8cpp)

# Unit tests for src/keymap.cpp -- key-chord names, --bind action lists in
# every delimiter form, the default keymap, --expect, KeyEvent -> Event.
add_executable(keymap_test tools/keymap_test.cpp src/keymap.cpp src/options.cpp)
target_include_directories(keymap_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(keymap_test PRIVATE utf8cpp)

# Unit tests for src/tokenizer.cpp -- fzf's tokenizer_test.go fixtures plus
# the --nth / --with-nth / --accept-nth transformer behaviour (T1.3).
add_executable(tokenizer_test tools/tokenizer_test.cpp src/tokenizer.cpp src/options.cpp)
target_include_directories(tokenizer_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(tokenizer_test PRIVATE utf8cpp)

# Unit tests for the item store and the matching engine (T1.4/T1.5):
# chunk list publication, ANSI stripping, the reader (header lines, cancel),
# merger ordering against a full sort, chunk-cache prefix narrowing, --tac,
# --no-sort and every tiebreak.
add_executable(search_test tools/search_test.cpp src/search.cpp src/matcher.cpp
    src/chunklist.cpp src/ansi.cpp src/tokenizer.cpp src/reader.cpp src/tty.cpp src/options.cpp)
target_include_directories(search_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(search_test PRIVATE utf8cpp Threads::Threads)

enable_testing()
add_test(NAME keyparser_test COMMAND keyparser_test)
add_test(NAME keymap_test COMMAND keymap_test)
add_test(NAME matcher_test COMMAND matcher_test)
add_test(NAME options_test COMMAND options_test)
add_test(NAME tokenizer_test COMMAND tokenizer_test)
add_test(NAME search_test COMMAND search_test)

# Standalone visual smoke test for src/render.hpp — prints a fake frame to
# the real terminal for manual eyeballing (no automated pixel comparison).
add_executable(render_smoketest tools/render_smoketest.cpp src/render.cpp src/tty.cpp)
target_include_directories(render_smoketest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(render_smoketest PRIVATE utf8cpp)

# Install target
install(TARGETS fzf DESTINATION bin)

# Print configuration summary
message(STATUS "")
message(STATUS "fzf++ Configuration Summary:")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Use System utfcpp: ${USE_SYSTEM_UTFCPP}")
message(STATUS "")
