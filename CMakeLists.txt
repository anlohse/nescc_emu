cmake_minimum_required(VERSION 3.16)
project(nes)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# emu6502 is a shared library, so on Windows its DLL has to sit next to anything
# that loads it. Sending every runtime artifact to one directory achieves that
# without per-target copy steps. Must be set before add_subdirectory() so the
# core's targets inherit it.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ------------------------------------------------------------------------------
# The 6502 core
# ------------------------------------------------------------------------------
# A sibling checkout is preferred, and fetched from git when there is not one.
#
# add_subdirectory() rather than find_package() so that editing the core rebuilds
# this too -- the NES work keeps surfacing changes there. The fetch fallback is
# what makes a plain "git clone && cmake" work for someone who only wants this
# repository; developing against a local core still just works, because the
# sibling directory wins whenever it exists. Its tests and debugger stay off.
set(EMU6502_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../emu6502" CACHE PATH
    "Path to a local emu6502 checkout; fetched from git if this does not exist")
set(EMU6502_GIT_REPOSITORY "https://github.com/anlohse/emu6205cc.git" CACHE STRING
    "Where to fetch the 6502 core when no local checkout is present")
set(EMU6502_GIT_TAG "main" CACHE STRING
    "Branch or tag of the 6502 core to fetch")

if(EXISTS "${EMU6502_DIR}/CMakeLists.txt")
    message(STATUS "Using the emu6502 checkout at ${EMU6502_DIR}")
    add_subdirectory("${EMU6502_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/emu6502")
else()
    message(STATUS "No emu6502 checkout at ${EMU6502_DIR}; fetching from "
                   "${EMU6502_GIT_REPOSITORY}")
    include(FetchContent)
    FetchContent_Declare(
        emu6502
        GIT_REPOSITORY "${EMU6502_GIT_REPOSITORY}"
        GIT_TAG "${EMU6502_GIT_TAG}"
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(emu6502)
endif()

# ------------------------------------------------------------------------------
# Console library
# ------------------------------------------------------------------------------
add_library(nes_lib STATIC
    src/Apu.cpp
    src/Cartridge.cpp
    src/Controller.cpp
    src/Mapper.cpp
    src/NesBus.cpp
    src/Nes.cpp
    src/Ppu.cpp
)

target_include_directories(nes_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(nes_lib PUBLIC emu6502::emu6502)

# ------------------------------------------------------------------------------
# Command-line runner
# ------------------------------------------------------------------------------
# Stays headless and dependency-free on purpose: tracing, scripted input and
# screenshots have to work where SDL is not available, which includes CI.
add_executable(nes_run src/main.cpp)
target_link_libraries(nes_run PRIVATE nes_lib)

# ------------------------------------------------------------------------------
# SDL2 window front-end
# ------------------------------------------------------------------------------
option(NES_BUILD_GUI "Build the SDL2 window front-end" ON)

if(NES_BUILD_GUI)
    include(FetchContent)

    # Prefer an installed SDL2; fall back to building one. The fallback is what
    # makes a fresh checkout work with nothing but a compiler and git.
    find_package(SDL2 QUIET)
    if(NOT SDL2_FOUND AND NOT TARGET SDL2::SDL2)
        message(STATUS "SDL2 not found; fetching it")
        set(SDL_TEST   OFF CACHE BOOL "" FORCE)
        set(SDL_SHARED ON  CACHE BOOL "" FORCE)
        set(SDL_STATIC OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            SDL2
            GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
            GIT_TAG release-2.30.9
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(SDL2)
    endif()

    add_executable(nes_gui WIN32 src/gui_main.cpp src/GuiConfig.cpp)
    target_link_libraries(nes_gui PRIVATE nes_lib SDL2::SDL2)

    # SDL renames main() to SDL_main and supplies the real entry point. On
    # Windows that is also what lets a WIN32 (non-console) executable start.
    if(TARGET SDL2::SDL2main)
        target_link_libraries(nes_gui PRIVATE SDL2::SDL2main)
    endif()
endif()

# ------------------------------------------------------------------------------
# Tests
# ------------------------------------------------------------------------------
option(NES_BUILD_TESTS "Build the NES test suite" ON)

if(NES_BUILD_TESTS)
    include(FetchContent)
    if(NOT TARGET doctest::doctest)
        FetchContent_Declare(
            doctest
            GIT_REPOSITORY https://github.com/doctest/doctest.git
            GIT_TAG v2.4.12
        )
        FetchContent_MakeAvailable(doctest)
    endif()

    enable_testing()

    add_executable(nes_test
        test_src/testMain.cpp
        test_src/testCartridge.cpp
        test_src/testApu.cpp
        test_src/testBatteryRam.cpp
        test_src/testBus.cpp
        test_src/testController.cpp
        test_src/testMappers.cpp
        test_src/testNestest.cpp
        test_src/testPpu.cpp
        test_src/testRegion.cpp
    )
    target_link_libraries(nes_test PRIVATE nes_lib doctest::doctest)

    # nestest.nes and its reference log are not redistributed here -- their
    # licensing is not clearly stated by the original author. Drop them into
    # nes/roms/ (or point this elsewhere) and the conformance test picks them up;
    # without them it reports itself as skipped.
    #
    #   nestest.nes  by Kevin Horton -- https://www.nesdev.org/wiki/Emulator_tests
    #   nestest.log  the reference trace published alongside it
    set(NES_TEST_ROM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/roms" CACHE PATH
        "Directory holding nestest.nes and nestest.log")

    if(EXISTS "${NES_TEST_ROM_DIR}/nestest.nes")
        target_compile_definitions(nes_test PRIVATE
            NES_TEST_ROM_DIR="${NES_TEST_ROM_DIR}")
        message(STATUS "nestest found in ${NES_TEST_ROM_DIR}")
    else()
        message(STATUS
            "nestest.nes not found in ${NES_TEST_ROM_DIR}; that test will be skipped. "
            "See the comment in nes/CMakeLists.txt for where to get it.")
    endif()

    add_test(NAME nes_test COMMAND nes_test)
endif()

# ------------------------------------------------------------------------------
# Front-end tests
# ------------------------------------------------------------------------------
# Kept out of nes_test so the emulator's own suite stays free of SDL and keeps
# building with -DNES_BUILD_GUI=OFF. Must come after both option() blocks above,
# or NES_BUILD_TESTS is simply undefined here and the target silently vanishes.
if(NES_BUILD_GUI AND NES_BUILD_TESTS)
    add_executable(nes_gui_test test_src/testGuiConfig.cpp src/GuiConfig.cpp)
    target_link_libraries(nes_gui_test PRIVATE SDL2::SDL2 doctest::doctest)
    if(TARGET SDL2::SDL2main)
        target_link_libraries(nes_gui_test PRIVATE SDL2::SDL2main)
    endif()
    add_test(NAME nes_gui_test COMMAND nes_gui_test)
endif()
