cmake_minimum_required(VERSION 3.16)

# The version stamped into the installer, and the one thing about a release that
# cannot be derived. A git tag is the real source of truth -- the release
# workflow passes the tag's version in here -- and this default is what a plain
# local build gets so that packaging works without one.
set(NES_VERSION "1.2.0" CACHE STRING "Version stamped into packages")
project(nes VERSION ${NES_VERSION})

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
set(EMU6502_GIT_REPOSITORY "https://github.com/anlohse/emu6502cc.git" CACHE STRING
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
    src/State.cpp
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

    add_executable(nes_gui WIN32
        src/gui_main.cpp
        src/GuiConfig.cpp
        src/frontend/App.cpp
        src/frontend/HostServices.cpp
        src/frontend/SdlBackend.cpp
        src/frontend/SdlPlugin.cpp
        src/frontend/PluginSettings.cpp
        src/frontend/MenuModel.cpp
        src/frontend/KeysReference.cpp
        src/frontend/CrtFilter.cpp
        src/frontend/PadMapping.cpp
        src/frontend/MenuBarWin32.cpp
        src/frontend/MenuBarStub.cpp
        src/frontend/FileDialogWin32.cpp
        src/frontend/FileDialogStub.cpp
        src/frontend/SettingsDialogWin32.cpp
        src/frontend/SettingsDialogStub.cpp
        src/frontend/BindingModel.cpp
        src/frontend/BindingsDialogWin32.cpp
        src/frontend/BindingsDialogStub.cpp
        src/plugin/PluginHost.cpp
        src/plugin/Module.cpp
    )
    target_link_libraries(nes_gui PRIVATE nes_lib SDL2::SDL2)

    # SDL renames main() to SDL_main and supplies the real entry point. On
    # Windows that is also what lets a WIN32 (non-console) executable start.
    if(TARGET SDL2::SDL2main)
        target_link_libraries(nes_gui PRIVATE SDL2::SDL2main)
    endif()

    # --------------------------------------------------------------------------
    # Loadable plugins
    # --------------------------------------------------------------------------
    # Built into a plugins/ folder beside the executable, which is where the
    # host looks. Deliberately not linked against nes_lib: a plugin talks to the
    # host through the C ABI and nothing else, and keeping the emulator core out
    # of its link line is what proves it.
    add_library(nes_audio_sdl MODULE src/plugins/audio_sdl/audio_sdl.cpp)
    target_link_libraries(nes_audio_sdl PRIVATE SDL2::SDL2)
    set_target_properties(nes_audio_sdl PROPERTIES
        OUTPUT_NAME "audio_sdl"
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins"
    )
    foreach(config Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER ${config} CONFIG_UPPER)
        set_target_properties(nes_audio_sdl PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY_${CONFIG_UPPER} "${CMAKE_BINARY_DIR}/bin/${config}/plugins"
            RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER} "${CMAKE_BINARY_DIR}/bin/${config}/plugins"
        )
    endforeach()
    add_dependencies(nes_gui nes_audio_sdl)
endif()

# ------------------------------------------------------------------------------
# Tests
# ------------------------------------------------------------------------------
option(NES_BUILD_TESTS "Build the NES test suite" ON)

if(NES_BUILD_TESTS)
    include(FetchContent)
    if(NOT TARGET doctest::doctest)
        # And do not let the test framework install itself. Built here through
        # FetchContent, its install() rules join this build, and a package made
        # from them carries doctest's headers and CMake config files inside the
        # emulator. The component filter below catches this too; both are cheap,
        # and stopping it at the source keeps the package honest even for a
        # generator that ignores components.
        set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
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
        test_src/testState.cpp
    )
    target_link_libraries(nes_test PRIVATE nes_lib doctest::doctest)

    # nestest.nes and its reference log are not redistributed here -- their
    # licensing is not clearly stated by the original author. Drop them into
    # nes/roms/ (or point this elsewhere) and the conformance test picks them up;
    # without them it reports itself as skipped.
    #
    #   nestest.nes  by Kevin Horton -- https://www.nesdev.org/wiki/Emulator_tests
    #   nestest.log  the reference trace published alongside it
    #
    # nes_rom_test below looks in <dir>/blargg for blargg's suites, in any
    # arrangement of subdirectories. They are published for exactly this
    # purpose and are collected at https://github.com/christopherpow/nes-test-roms
    # -- ppu_vbl_nmi, apu_test, mmc3_test and instr_test-v5 are the ones that
    # earn their keep first.
    set(NES_TEST_ROM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/roms" CACHE PATH
        "Directory holding nestest.nes, nestest.log and a blargg/ subdirectory")

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

    # Kept out of nes_test because it is a different kind of test: minutes of
    # emulated console time rather than milliseconds, and it depends on ROMs
    # that are not in this repository. The fast suite has to stay fast enough
    # to run on every save.
    add_executable(nes_rom_test test_src/testBlargg.cpp)
    target_link_libraries(nes_rom_test PRIVATE nes_lib doctest::doctest)
    if(IS_DIRECTORY "${NES_TEST_ROM_DIR}/blargg")
        message(STATUS "test ROMs found in ${NES_TEST_ROM_DIR}/blargg")
    else()
        message(STATUS
            "no test ROMs in ${NES_TEST_ROM_DIR}/blargg; nes_rom_test will skip. "
            "See the comment in nes/CMakeLists.txt for where to get them.")
    endif()
    target_compile_definitions(nes_rom_test PRIVATE
        NES_TEST_ROM_DIR="${NES_TEST_ROM_DIR}")
    add_test(NAME nes_rom_test COMMAND nes_rom_test)
endif()

# ------------------------------------------------------------------------------
# Front-end tests
# ------------------------------------------------------------------------------
# Kept out of nes_test so the emulator's own suite stays free of SDL and keeps
# building with -DNES_BUILD_GUI=OFF. Must come after both option() blocks above,
# or NES_BUILD_TESTS is simply undefined here and the target silently vanishes.
if(NES_BUILD_GUI AND NES_BUILD_TESTS)
    # App.cpp is here but SdlBackend.cpp is not: the run loop is tested against
    # doubles, which is the whole point of the backend interfaces.
    add_executable(nes_gui_test
        test_src/testGuiConfig.cpp
        test_src/testApp.cpp
        test_src/testPlugin.cpp
        test_src/testPluginSettings.cpp
        test_src/testBindingModel.cpp
        test_src/testHostServices.cpp
        test_src/testMenuModel.cpp
        test_src/testKeysReference.cpp
        test_src/testCrtFilter.cpp
        test_src/testPadMapping.cpp
        src/GuiConfig.cpp
        src/frontend/MenuModel.cpp
        src/frontend/KeysReference.cpp
        src/frontend/CrtFilter.cpp
        src/frontend/PadMapping.cpp
        src/frontend/App.cpp
        src/frontend/HostServices.cpp
        src/frontend/PluginSettings.cpp
        src/frontend/BindingModel.cpp
        src/frontend/BindingsDialogWin32.cpp
        src/frontend/BindingsDialogStub.cpp
        src/plugin/PluginHost.cpp
        src/plugin/Module.cpp
    )
    target_link_libraries(nes_gui_test PRIVATE nes_lib SDL2::SDL2 doctest::doctest)
    if(TARGET SDL2::SDL2main)
        target_link_libraries(nes_gui_test PRIVATE SDL2::SDL2main)
    endif()
    # The loader tests open the real audio plugin, so it has to exist first.
    add_dependencies(nes_gui_test nes_audio_sdl)
    add_test(NAME nes_gui_test COMMAND nes_gui_test)
endif()

# ------------------------------------------------------------------------------
# Packaging
# ------------------------------------------------------------------------------
# An MSI for Windows, plus the same tree as a ZIP for people who would rather
# not install anything.
#
# The contents are described once, here, as install() rules -- not as a list of
# files to copy in a release script. A copy list in a workflow is a second
# description of the same thing, and the two drift the moment a plugin or a DLL
# is added: the build keeps working, the package quietly ships without it, and
# nothing fails until somebody runs the installed copy.
if(NES_BUILD_GUI AND WIN32)
    # Flat, because this is a program somebody double-clicks rather than a
    # library somebody links. The plugins folder is the one piece of structure
    # that matters, and it has to survive because that is where the host looks.
    install(TARGETS nes_gui nes_run RUNTIME DESTINATION . COMPONENT runtime)
    install(TARGETS nes_audio_sdl
        LIBRARY DESTINATION plugins COMPONENT runtime
        RUNTIME DESTINATION plugins COMPONENT runtime)
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/ROADMAP.md"
        DESTINATION . COMPONENT runtime)

    # SDL2 and the 6502 core are shared libraries, so the program does not start
    # without them sitting alongside it. Asking CMake which DLLs the executable
    # actually resolves to is better than naming them: it cannot be out of date,
    # and it keeps working when SDL is found on the machine instead of built here.
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.21)
        install(FILES $<TARGET_RUNTIME_DLLS:nes_gui>
            DESTINATION . COMPONENT runtime)
    else()
        # Named explicitly for older CMake, which has no way to ask.
        install(FILES $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE:emu6502::emu6502>
            DESTINATION . COMPONENT runtime)
    endif()

    # WiX shows the licence in its own dialog and wants RTF; CPack will convert a
    # .txt for us, but only if it can see that extension. The repository's file
    # has none, so it is copied to one that does.
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt" COPYONLY)

    set(CPACK_PACKAGE_NAME "nescc_emu")
    set(CPACK_PACKAGE_VENDOR "Alan N. Lohse")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "A NES emulator built on the emu6502 core")
    set(CPACK_PACKAGE_VERSION "${NES_VERSION}")
    set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/anlohse/nescc_emu")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt")

    # The same name the release has always used, so a link to an older download
    # and a link to a new one look like the same project.
    set(CPACK_PACKAGE_FILE_NAME "nescc_emu-${NES_VERSION}-windows-x64")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "nescc_emu")

    # A Start Menu entry for the window, and none for the headless runner --
    # nes_run is a command-line tool and a shortcut to it would open and vanish.
    set(CPACK_PACKAGE_EXECUTABLES "nes_gui;NES emulator")
    set(CPACK_CREATE_DESKTOP_LINKS "nes_gui")

    # This GUID identifies the *product line* rather than any one build, and is
    # what lets an MSI replace the version already installed instead of
    # installing beside it. It must therefore never change again: a new GUID
    # would leave every previously installed copy on the machine with no way to
    # remove it except by hand.
    set(CPACK_WIX_UPGRADE_GUID "ACC992E0-3D62-4E0A-BA44-49C998681226")
    set(CPACK_WIX_PROGRAM_MENU_FOLDER "nescc_emu")
    set(CPACK_WIX_PROPERTY_ARPHELPLINK "${CPACK_PACKAGE_HOMEPAGE_URL}")
    set(CPACK_WIX_PROPERTY_ARPURLINFOABOUT "${CPACK_PACKAGE_HOMEPAGE_URL}")

    # WIX is what was asked for; ZIP alongside it costs nothing and is the same
    # tree, which is worth having for anyone who cannot or would rather not run
    # an installer. Both come from the install() rules above, so they cannot
    # disagree about what is in the package.
    set(CPACK_GENERATOR "WIX;ZIP")

    # Only what this project installs, and this is not a nicety. SDL2 and doctest
    # are built here through FetchContent, which brings their own install() rules
    # into this build -- so a package built from "everything installable" shipped
    # doctest's headers and CMake config files inside the emulator. Naming one
    # component and packaging only that excludes anything a dependency installs,
    # including dependencies added later, rather than removing them one at a time
    # as they are noticed.
    set(CPACK_COMPONENTS_ALL runtime)
    set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
    # The archive generators ignore components entirely unless this is on -- they
    # default to installing everything and naming no component at all, which is
    # how the first ZIP came out with doctest inside it despite the list above.
    # WiX reads the component list without being asked.
    set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
    # And put the files inside a folder named like the archive, which component
    # packaging otherwise drops. Without it the ZIP unpacks nine loose files into
    # whatever directory it was opened in.
    #
    # The MSI wants the opposite, and turns this back off in CPackOptions.cmake
    # below: an installer has a directory of its own already, and a versioned
    # folder nested inside it would move the program on every upgrade.
    set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY ON)
    set(CPACK_PROJECT_CONFIG_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CPackOptions.cmake")
    # Nothing for a person to choose in the installer: there is one program here,
    # and a feature tree with a single feature in it is only a page to click past.
    set(CPACK_COMPONENT_RUNTIME_HIDDEN TRUE)
    set(CPACK_COMPONENT_RUNTIME_REQUIRED TRUE)
    set(CPACK_COMPONENT_RUNTIME_DISPLAY_NAME "nescc_emu")

    include(CPack)
endif()
