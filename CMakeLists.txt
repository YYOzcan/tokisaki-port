cmake_minimum_required(VERSION 3.20)
project(tokisaki-port
    VERSION 0.1.0
    DESCRIPTION "Killzone 2 PC Port (PS3 Recompilation Framework)"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler Warnings and Optimizations
if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

# Find Threads
find_package(Threads REQUIRED)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Runtime Sources
set(RUNTIME_SOURCES
    src/common/types.hpp
    src/common/endian.hpp
    src/runtime/memory.hpp
    src/runtime/memory.cpp
    src/runtime/spu_manager.hpp
    src/runtime/spu_manager.cpp
    src/runtime/runtime.hpp
    src/runtime/runtime.cpp
)

add_library(tokisaki_runtime STATIC ${RUNTIME_SOURCES})
target_link_libraries(tokisaki_runtime PUBLIC Threads::Threads)

# Main Executable
add_executable(tokisaki-port
    src/main.cpp
)

target_link_libraries(tokisaki-port PRIVATE tokisaki_runtime)

# Configuration summary
message(STATUS "--------------------------------------------------------")
message(STATUS "  Tokisaki Port (Killzone 2 PC Port - PS3 Recomp)")
message(STATUS "  Target Architecture: Host (x86_64)")
message(STATUS "  Guest Architecture: Cell Broadband Engine (PPU/SPU)")
message(STATUS "  C++ Standard: 20")
message(STATUS "--------------------------------------------------------")
