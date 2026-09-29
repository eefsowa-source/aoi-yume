add_executable(rompler_tests
    test_plugin_smoke.cpp
    test_sf2_playback.cpp
    test_dsp_behaviors.cpp
    test_sampler_quality.cpp
    "${CMAKE_SOURCE_DIR}/libs/instrument/test/support/AllocationGuard.cpp"
    test_bus_processor.cpp
    test_dynamics_processor.cpp
    test_preset_storage.cpp
    test_soundfont_resolver.cpp
    test_preset_library.cpp
    test_processor_presets.cpp
    test_preset_browser_overlay.cpp
    test_editor_resize.cpp
    test_knob_init_state.cpp
    test_editor_interaction.cpp
    test_editor_preset_header.cpp
    test_output_safety.cpp
    test_stereo_render.cpp
    test_output_headroom_benchmark.cpp
    test_full_processor_cpu.cpp)

# EONDS50 carries the JucePlugin_* definitions PUBLIC, but JUCE modules
# are linked PRIVATE on the shared-code target by convention, so their include
# directories do not propagate. The test needs the module explicitly.
target_link_libraries(rompler_tests PRIVATE
    EONDS50
    juce::juce_audio_utils
    juce::juce_dsp
    x10::dsp
    x10::sf2
    x10::instrument
    Catch2::Catch2)

target_include_directories(rompler_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../Source"
    "${CMAKE_SOURCE_DIR}/libs/instrument/test")

target_compile_options(rompler_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -Wshadow)

# Points at the local, untracked SF2 corpus (see the top-level .gitignore).
# It lives outside this worktree, in the main checkout, since it is never
# committed. Tests that need it skip cleanly when the directory is absent.
target_compile_definitions(rompler_tests PRIVATE
    X10_SF2_CROSSCHECK_TESTDATA="${CMAKE_SOURCE_DIR}/testdata/sf2"
    AOD_ROMPLER_FACTORY_PRESET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../Resources/Presets")

catch_discover_tests(rompler_tests)
