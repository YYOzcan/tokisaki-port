# Tokisaki Port -- Killzone 2 (BCUS98116) on ps3recomp.
#
# The lifted game code is generated locally from your own EBOOT.ELF and is
# never committed (see tools/lift.sh). This file wires it to the ps3recomp
# runtime, built from external/ps3recomp as a subdirectory.
#
#   tools/lift.sh                                    # out/ + recompiled/
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
#         -DPS3RECOMP_RSX_VULKAN=ON
#   cmake --build build -j4
#   tools/run.sh
cmake_minimum_required(VERSION 3.20)
project(tokisaki-port
    VERSION 0.2.0
    DESCRIPTION "Killzone 2 PC port (ps3recomp static recompilation)"
    LANGUAGES C CXX
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/ps3recomp" CACHE PATH
    "ps3recomp toolkit checkout")
set(KZ2_RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/recompiled" CACHE PATH
    "ppu_lifter.py output: ppu_recomp.h and ppu_recomp_NNN.cpp")
set(KZ2_SPU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/spu_recompiled" CACHE PATH
    "spu_lifter.py output: one <image>/spu_recomp.c per SPU image")
# -O1 keeps the compiler's memory per 150k-line chunk around 2 GB.
# The lifted SPU jobs are the in-level bottleneck (Killzone 2's post-processing
# runs on one SPU); -O3 lets the vector helpers vectorize and inline.
set(KZ2_SPU_OPT "-O3" CACHE STRING "optimisation level for the lifted SPU images")
option(KZ2_SPU_LS_DIAG "LS probes and watches (SPU_LS_WATCH, ...) in the lifted SPU code" OFF)
# Profile-guided optimisation of the lifted SPU code: "gen" instruments it
# (run with KZ2_PGO=<start>,<end> frames; profiles land in build/pgo), then
# llvm-profdata merge -o pgo/spu.profdata pgo/*.profraw and "use".
set(KZ2_SPU_PGO "off" CACHE STRING "off | gen | use")
set(KZ2_RECOMP_COMPILE_OPTIONS "-O1" CACHE STRING
    "extra compile options for the lifted PPU chunks")

# The lifted SPU code is mostly 128-bit vector work: shufb, fa/fm/fma. Its
# helpers (spu_helpers.h) only take their SSE4.1 forms when the compiler may
# use them, and the default x86-64 baseline fell back to a byte loop for every
# shufb -- 40% of the SPU time in the first level. x86-64-v3 (AVX2, FMA;
# Haswell / Excavator and later) is the default; set KZ2_ARCH=x86-64-v2 for an
# SSE4.2-only machine, or native for this one.
set(KZ2_ARCH "x86-64-v3" CACHE STRING "-march for the game and the runtime")
if(KZ2_ARCH AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    add_compile_options(-march=${KZ2_ARCH})
endif()

if(NOT EXISTS "${PS3RECOMP_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "ps3recomp not found at ${PS3RECOMP_DIR}. "
                        "Run: git submodule update --init")
endif()
if(NOT EXISTS "${KZ2_RECOMP_DIR}/ppu_recomp.h")
    message(FATAL_ERROR "No lifted code in ${KZ2_RECOMP_DIR}. Run tools/lift.sh first.")
endif()

add_subdirectory("${PS3RECOMP_DIR}" ps3recomp EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Generated inputs
# ---------------------------------------------------------------------------
file(GLOB KZ2_RECOMP_SOURCES CONFIGURE_DEPENDS "${KZ2_RECOMP_DIR}/ppu_recomp_*.cpp")
list(LENGTH KZ2_RECOMP_SOURCES KZ2_RECOMP_COUNT)
message(STATUS "Killzone 2 lifted PPU chunks: ${KZ2_RECOMP_COUNT}")

find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(KZ2_HLE_NIDS "${CMAKE_CURRENT_BINARY_DIR}/ppu_hle_nids.cpp")
add_custom_command(
    OUTPUT "${KZ2_HLE_NIDS}"
    COMMAND "${Python3_EXECUTABLE}" -I "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
            --out "${KZ2_HLE_NIDS}" --all
    DEPENDS "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
            "${PS3RECOMP_DIR}/tools/nid_database.py"
    COMMENT "Generating the HLE NID -> handler table")

set(KZ2_SPU_SOURCES "")
file(GLOB KZ2_SPU_LIFTED CONFIGURE_DEPENDS "${KZ2_SPU_DIR}/*/spu_recomp.c")
# tools/spu_regcache.py writes spu_recomp_rc.c next to each image: the same
# functions with the SPU registers in locals instead of the context in memory.
# Off: here it is slower (PROGRESS 62) -- clang already keeps ctx->gpr in
# registers, and the locals of an 80-register loop spill to the stack.
option(KZ2_SPU_REGCACHE "Build the register-cached SPU sources (spu_recomp_rc.c) where present" OFF)
if(KZ2_SPU_REGCACHE)
    set(_kz2_spu "")
    foreach(src IN LISTS KZ2_SPU_LIFTED)
        get_filename_component(dir "${src}" DIRECTORY)
        if(EXISTS "${dir}/spu_recomp_rc.c")
            list(APPEND _kz2_spu "${dir}/spu_recomp_rc.c")
        else()
            list(APPEND _kz2_spu "${src}")
        endif()
    endforeach()
    set(KZ2_SPU_LIFTED ${_kz2_spu})
endif()
if(KZ2_SPU_LIFTED)
    list(APPEND KZ2_SPU_SOURCES ${KZ2_SPU_LIFTED}
         "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
    foreach(gen IN ITEMS spu_workloads.c jobmods_register.c)
        if(EXISTS "${KZ2_SPU_DIR}/${gen}")
            list(APPEND KZ2_SPU_SOURCES "${KZ2_SPU_DIR}/${gen}")
        endif()
    endforeach()
    if(EXISTS "${KZ2_SPU_DIR}/pm_DDD59DC528B4CED6/spu_recomp.c")
        list(APPEND KZ2_SPU_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/kz2_spu_pm.c")
    endif()
    list(LENGTH KZ2_SPU_LIFTED KZ2_SPU_COUNT)
    message(STATUS "Killzone 2 lifted SPU images: ${KZ2_SPU_COUNT}")
endif()

# ---------------------------------------------------------------------------
# The game
# ---------------------------------------------------------------------------
file(GLOB KZ2_LIB_INC_DIRS LIST_DIRECTORIES true "${PS3RECOMP_DIR}/libs/*")

option(KZ2_FTRACE "Instrument the lifted chunks for src/kz2_ftrace.cpp (diagnostics)" OFF)

add_executable(killzone2
    src/boot_main.cpp
    src/kz2_hle.cpp
    src/kz2_ftrace.cpp
    src/kz2_fiber.cpp
    src/kz2_mouse.cpp
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"
    "${KZ2_HLE_NIDS}"
    ${KZ2_RECOMP_SOURCES}
    ${KZ2_SPU_SOURCES}
)

target_include_directories(killzone2 PRIVATE
    "${KZ2_RECOMP_DIR}"
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/memory"
    "${PS3RECOMP_DIR}/runtime/syscalls"
    "${PS3RECOMP_DIR}/runtime/platform"
    ${KZ2_LIB_INC_DIRS}
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${PS3RECOMP_DIR}/libs/font"              # stb_truetype.h (glyph outlines)
)

find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)        # Edge Zlib requests are inflated on the host
target_link_libraries(killzone2 PRIVATE ps3recomp_runtime Threads::Threads ZLIB::ZLIB ${CMAKE_DL_LIBS})
set_target_properties(killzone2 PROPERTIES LINKER_LANGUAGE CXX)

if(MSVC)
    target_compile_options(killzone2 PRIVATE /W3 /wd4100 /wd4244 /wd4267)
    target_link_libraries(killzone2 PRIVATE dbghelp user32 gdi32 winmm)
    target_link_options(killzone2 PRIVATE /STACK:268435456)
    set_source_files_properties(${KZ2_RECOMP_SOURCES} PROPERTIES COMPILE_OPTIONS "/w;/bigobj")
else()
    target_compile_options(killzone2 PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing)
    # Generated code: no warnings, and no fused multiply-add the guest did not ask for.
    set(KZ2_RECOMP_FLAGS "-w;-ffp-contract=off;${KZ2_RECOMP_COMPILE_OPTIONS}")
    if(KZ2_FTRACE)
        list(APPEND KZ2_RECOMP_FLAGS -finstrument-functions)
    endif()
    set_source_files_properties(${KZ2_RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "${KZ2_RECOMP_FLAGS}")
endif()

set(KZ2_SPU_PGO_FLAGS "")
if(KZ2_SPU_PGO STREQUAL "gen")
    set(KZ2_SPU_PGO_FLAGS "-fprofile-generate=${CMAKE_BINARY_DIR}/pgo")
    target_link_options(killzone2 PRIVATE "-fprofile-generate=${CMAKE_BINARY_DIR}/pgo")
elseif(KZ2_SPU_PGO STREQUAL "use")
    set(KZ2_SPU_PGO_FLAGS "-fprofile-use=${CMAKE_SOURCE_DIR}/pgo/spu.profdata;-Wno-profile-instr-unprofiled;-Wno-profile-instr-out-of-date")
endif()
if(KZ2_SPU_SOURCES)
    set_source_files_properties(${KZ2_SPU_SOURCES} PROPERTIES
        INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu"
        # SPU_LS_DIAG=0: no LS probes/watches in the lifted code's local-store
        # accesses (the runtime keeps them); KZ2_SPU_LS_DIAG=ON to debug.
        COMPILE_DEFINITIONS "SPU_USE_DISPATCH_MT;SPU_LS_DIAG=$<IF:$<BOOL:${KZ2_SPU_LS_DIAG}>,1,0>"
        COMPILE_OPTIONS "-w;${KZ2_SPU_OPT};${KZ2_SPU_PGO_FLAGS}")
endif()

install(TARGETS killzone2 RUNTIME DESTINATION bin)
