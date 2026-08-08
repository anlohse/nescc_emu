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
# Consumed from a sibling checkout by default. add_subdirectory() rather than a
# package so that editing the core rebuilds this too -- the NES work is expected
# to keep surfacing changes there (a decimal-mode switch for the 2A03, an OAM DMA
# stall). Its tests and debugger stay off; build them from that project directly.
set(EMU6502_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../emu6502" CACHE PATH
    "Path to the emu6502 checkout")

if(NOT EXISTS "${EMU6502_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "emu6502 not found at ${EMU6502_DIR}. "
        "Check it out beside this directory, or pass -DEMU6502_DIR=<path>.")
endif()

add_subdirectory("${EMU6502_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/emu6502")

# ------------------------------------------------------------------------------
# Console library
# ------------------------------------------------------------------------------
add_library(nes_lib STATIC
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
add_executable(nes_run src/main.cpp)
target_link_libraries(nes_run PRIVATE nes_lib)

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
        test_src/testBus.cpp
        test_src/testController.cpp
        test_src/testNestest.cpp
        test_src/testPpu.cpp
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
