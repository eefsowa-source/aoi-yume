cmake_minimum_required(VERSION 3.22)

# C is required by JUCE module sources on macOS even though none of our own code
# uses it; without it the generate step fails on CMAKE_C_COMPILE_OBJECT.
project(AgeOfDistortion VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

option(X10_BUILD_TESTS   "Build unit tests"                          ON)
option(X10_BUILD_PLUGINS "Build the JUCE plugin targets (M0 onward)" OFF)
option(X10_SANITIZE      "Enable ASan + UBSan on x10 targets"        OFF)

# x10_dsp itself must never depend on JUCE (plan D1). JUCE is fetched only when
# a plugin target is actually being built, which keeps the DSP test cycle at
# under a second and keeps the licence boundary obvious.
set(X10_JUCE_TAG "8.0.14" CACHE STRING "JUCE version pin for plugin targets")

enable_testing()

add_subdirectory(libs/x10_dsp)
add_subdirectory(libs/instrument)
add_subdirectory(libs/sf2)
add_subdirectory(tools/sf2_crosscheck)

if(X10_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.7.1
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(Catch2)

    # Fetched at top level so that CMAKE_MODULE_PATH and the Catch module reach
    # every test directory. Declaring them inside one test subdirectory leaves
    # sibling directories without catch_discover_tests.
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
    include(Catch)

    add_subdirectory(libs/x10_dsp/test)
    add_subdirectory(libs/instrument/test)
    add_subdirectory(libs/sf2/test)
endif()

if(X10_BUILD_PLUGINS)
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        ${X10_JUCE_TAG}
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)

    FetchContent_Declare(melatonin_inspector
        GIT_REPOSITORY https://github.com/sudara/melatonin_inspector.git
        GIT_TAG 9c483f865854d83fe9873e1c20e3fd772f695f80
        # melatonin's CMakeLists assumes its source dir is literally named
        # "melatonin_inspector" (juce_add_module derives the module name from
        # the directory). The FetchContent default <name>-src path breaks that,
        # so pin the checkout directory name.
        SOURCE_DIR "${FETCHCONTENT_BASE_DIR}/melatonin_inspector"
        GIT_SHALLOW FALSE)
    FetchContent_MakeAvailable(melatonin_inspector)

    add_subdirectory(plugins/rompler)
endif()
