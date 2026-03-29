cmake_minimum_required(VERSION 3.14)
project(fzf++ VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Darwin-specific notes:
# Bundled FTXUI allows new C++ ABI on libstdc++-using systems.
# If you use a port for FTXUI and thus old ABI, you will likely find it useful to invoke
# the executable via a wrapper that sets DYLD_LIBRARY_PATH to libgcc location.
# See: https://github.com/macos-powerpc/powerpc-ports/blob/64891b48c96b37b529c25328d3a3d51b8126af78/_resources/port1.0/group/legacysupport-1.1.tcl#L244-L267
# Also, using bundled dependencies introduces build-time dependency on git.

option(USE_SYSTEM_CLI11 "Use system CLI11 library" OFF)
option(USE_SYSTEM_FTXUI "Use system FTXUI library" OFF)
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

# CLI11 - Header-only command line parser
if(USE_SYSTEM_CLI11)
    find_package(CLI11 CONFIG REQUIRED)
else()
    # Use bundled CLI11
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/CLI11/include/CLI/CLI.hpp")
        message(STATUS "CLI11 not found, fetching the source")
        include(FetchContent)
        FetchContent_Declare(
            cli11
            GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
            GIT_TAG v2.4.1
        )
        FetchContent_MakeAvailable(cli11)
    else()
        add_subdirectory(external/CLI11 EXCLUDE_FROM_ALL)
    endif()
endif()

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

# FTXUI - Terminal UI library
if(USE_SYSTEM_FTXUI)
    find_package(ftxui CONFIG REQUIRED)
else()
    # Use bundled FTXUI
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/ftxui/CMakeLists.txt")
        message(STATUS "FTXUI not found, fetching the source")
        include(FetchContent)
        FetchContent_Declare(
            ftxui
            GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
            GIT_TAG v5.0.0
        )
        FetchContent_MakeAvailable(ftxui)
    else()
        set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
        add_subdirectory(external/ftxui EXCLUDE_FROM_ALL)
    endif()
endif()

# Source files
set(SOURCES
    src/main.cpp
    src/options.cpp
    src/matcher.cpp
    src/reader.cpp
    src/terminal.cpp
)

# Main executable
add_executable(fzf ${SOURCES})

target_include_directories(fzf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(fzf PRIVATE
    CLI11::CLI11
    utf8cpp
    ftxui::screen
    ftxui::dom
    ftxui::component
    Threads::Threads
)

# Platform-specific libraries
if(APPLE)
    # On < 10.7 you will need legacy-support for getline.
elseif(UNIX)
    # Linux may need additional libraries
    target_link_libraries(fzf PRIVATE rt)
endif()

# Install target
install(TARGETS fzf DESTINATION bin)

# Print configuration summary
message(STATUS "")
message(STATUS "fzf++ Configuration Summary:")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Use System CLI11: ${USE_SYSTEM_CLI11}")
message(STATUS "  Use System utfcpp: ${USE_SYSTEM_UTFCPP}")
message(STATUS "  Use System FTXUI: ${USE_SYSTEM_FTXUI}")
message(STATUS "")
