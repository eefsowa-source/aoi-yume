juce_add_plugin(EONDS50
    PLUGIN_MANUFACTURER_CODE    EonL
    PLUGIN_CODE                 AoYu
    FORMATS                     VST3 AU Standalone
    PRODUCT_NAME                "Aoi YUME"
    DESCRIPTION                 "Polyphonic SF2 rompler with per-voice nonlinear drive"
    COMPANY_NAME                "EON LAB"
    BUNDLE_ID                   com.eonlab.aoi-yume
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD     FALSE
    AU_MAIN_TYPE                kAudioUnitType_MusicDevice
    AU_SANDBOX_SAFE             TRUE)

juce_add_binary_data(AoiYumeSkinData
    SOURCES
        Resources/AoiYumeBlueDream.png)

target_sources(EONDS50 PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/PresetBrowserOverlay.cpp
    Source/Sampler.cpp
    Source/BandLimitedInterpolator.cpp
    Source/SF2Loader.cpp
    Source/BusProcessor.cpp
    Source/DynamicsProcessor.cpp
    Source/OutputSafetyProcessor.cpp
    Source/FxProcessor.cpp
    Source/PresetStorage.cpp
    Source/SoundFontResolver.cpp
    Source/PresetLibrary.cpp)
target_include_directories(EONDS50 PRIVATE Source)

target_compile_definitions(EONDS50 PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

# NOTE: JUCE_DISPLAY_SPLASH_SCREEN is deliberately left at its default. Turning
# it off requires a paid JUCE licence, and the licence tier for this project is
# still unresolved in the planning document. Setting it now would be asserting a
# licence we may not hold.

target_link_libraries(EONDS50
    PRIVATE
        AoiYumeSkinData
        x10::dsp
        x10::sf2
        x10::instrument
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
target_link_libraries(EONDS50 PUBLIC "$<$<CONFIG:Debug>:melatonin_inspector>")
target_compile_definitions(EONDS50 PUBLIC "$<$<CONFIG:Debug>:MELATONIN_INSPECTOR_ENABLED=1>")

# Headless UI snapshot tool. Renders the real editor offscreen to a PNG so the
# deck can be inspected without a host window (and without Screen Recording
# permissions, which this machine does not grant to the terminal).
if(APPLE OR UNIX)
    add_executable(ui_shot tools/ui_shot.cpp)
    target_link_libraries(ui_shot PRIVATE
        EONDS50
        x10::dsp
        x10::sf2
        x10::instrument
        juce::juce_audio_utils
        juce::juce_dsp)
    target_include_directories(ui_shot PRIVATE Source)
    target_compile_options(ui_shot PRIVATE -Wall -Wextra -Wpedantic -Werror -Wshadow)
endif()

# Our own sources are held to the same bar as x10_dsp, minus the numeric
# warnings that JUCE's own headers do not compile cleanly under.
target_compile_options(EONDS50 PRIVATE -Wall -Wextra -Wpedantic -Werror -Wshadow)

# --------------------------------------------------------------------------
# Bundled SoundFonts
#
# Three SoundFonts are shipped inside the plugin bundle's
# Contents/Resources/SoundFonts directory so the plugin is immediately
# playable without a manual Load step. Crystal Legacy is the
# deterministic startup default (matched by exact filename at runtime); the
# other two are simply available in the bundled-fonts picker. Missing sources
# fail the configure step loudly rather than silently shipping an incomplete
# bundle.
# --------------------------------------------------------------------------
set(EONDS50_BUNDLED_SF2_CRYSTAL_LEGACY
    "${CMAKE_CURRENT_SOURCE_DIR}/../../testdata/sf2/Crystal Legacy.sf2")
set(EONDS50_BUNDLED_SF2_NATURAL_STAGE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../testdata/sf2/Natural Stage.sf2")
set(EONDS50_BUNDLED_SF2_STUDIO_ESSENTIALS
    "${CMAKE_CURRENT_SOURCE_DIR}/../../testdata/sf2/Studio Essentials.sf2")
set(EONDS50_FACTORY_PRESET_CLEAR
    "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Presets/Aoi Clear.eonpreset")
set(EONDS50_FACTORY_PRESET_CHARACTER
    "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Presets/Aoi Character.eonpreset")

foreach(sf2file
        "${EONDS50_BUNDLED_SF2_CRYSTAL_LEGACY}"
        "${EONDS50_BUNDLED_SF2_NATURAL_STAGE}"
        "${EONDS50_BUNDLED_SF2_STUDIO_ESSENTIALS}")
    if(NOT EXISTS "${sf2file}")
        message(FATAL_ERROR "Missing bundled SoundFont: ${sf2file}")
    endif()
endforeach()

foreach(presetfile
        "${EONDS50_FACTORY_PRESET_CLEAR}"
        "${EONDS50_FACTORY_PRESET_CHARACTER}")
    if(NOT EXISTS "${presetfile}")
        message(FATAL_ERROR "Missing factory profile: ${presetfile}")
    endif()
endforeach()

if(APPLE)
    foreach(fmt VST3 AU Standalone)
        if(TARGET EONDS50_${fmt})
            add_custom_command(TARGET EONDS50_${fmt} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
                        "$<GENEX_EVAL:$<TARGET_PROPERTY:EONDS50_${fmt},JUCE_PLUGIN_ARTEFACT_FILE>>/Contents/Resources/SoundFonts"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${EONDS50_BUNDLED_SF2_CRYSTAL_LEGACY}"
                        "${EONDS50_BUNDLED_SF2_NATURAL_STAGE}"
                        "${EONDS50_BUNDLED_SF2_STUDIO_ESSENTIALS}"
                        "$<GENEX_EVAL:$<TARGET_PROPERTY:EONDS50_${fmt},JUCE_PLUGIN_ARTEFACT_FILE>>/Contents/Resources/SoundFonts/"
                COMMAND ${CMAKE_COMMAND} -E make_directory
                        "$<GENEX_EVAL:$<TARGET_PROPERTY:EONDS50_${fmt},JUCE_PLUGIN_ARTEFACT_FILE>>/Contents/Resources/Presets"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${EONDS50_FACTORY_PRESET_CLEAR}"
                        "${EONDS50_FACTORY_PRESET_CHARACTER}"
                        "$<GENEX_EVAL:$<TARGET_PROPERTY:EONDS50_${fmt},JUCE_PLUGIN_ARTEFACT_FILE>>/Contents/Resources/Presets/"
                COMMENT "Bundling SoundFonts and profiles into ${fmt}"
                VERBATIM)
        endif()
    endforeach()
endif()

# --------------------------------------------------------------------------
# Ad-hoc signing
#
# macOS refuses to load an unsigned component, and only an "Apple Development"
# identity is available on this machine — there is no "Developer ID Application"
# certificate, so notarised distribution signing is not possible yet and is
# deferred to M6. Ad-hoc signing is enough for local hosts and for pluginval.
# --------------------------------------------------------------------------
#
# JUCE_PLUGIN_ARTEFACT_FILE holds a generator expression as its *value*, so
# reading it yields another unevaluated genex. It has to be run through
# GENEX_EVAL or codesign receives the literal "$<TARGET_BUNDLE_DIR:...>".
#
# This step must come last. JUCE runs its own signature check and then writes
# moduleinfo.json into the VST3 bundle, which invalidates whatever signature was
# there; signing afterwards is what makes the bundle actually loadable.
if(APPLE)
    foreach(format VST3 AU Standalone)
        if(TARGET EONDS50_${format})
            add_custom_command(TARGET EONDS50_${format} POST_BUILD
                COMMAND codesign --force --sign - --timestamp=none
                        "$<GENEX_EVAL:$<TARGET_PROPERTY:EONDS50_${format},JUCE_PLUGIN_ARTEFACT_FILE>>"
                COMMENT "Ad-hoc signing ${format}"
                VERBATIM)
        endif()
    endforeach()
endif()

# --------------------------------------------------------------------------
# pluginval — Gate 3
# --------------------------------------------------------------------------
find_program(PLUGINVAL_EXECUTABLE
    NAMES pluginval
    HINTS "/Applications/pluginval.app/Contents/MacOS"
          "$ENV{HOME}/Applications/pluginval.app/Contents/MacOS")

if(PLUGINVAL_EXECUTABLE)
    add_test(NAME pluginval_vst3_strictness10
        COMMAND "${PLUGINVAL_EXECUTABLE}"
                --strictness-level 10
                --timeout-ms 300000
                # Same GENEX_EVAL requirement as the signing step above: the
                # property's value is itself a generator expression.
                --validate "$<GENEX_EVAL:$<TARGET_PROPERTY:EONDS50_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>")
    set_tests_properties(pluginval_vst3_strictness10 PROPERTIES TIMEOUT 600)
else()
    message(WARNING "pluginval not found — Gate 3 will not run. "
                    "Install it to /Applications for the M0 acceptance test.")
endif()

if(X10_BUILD_TESTS)
    add_subdirectory(test)
endif()
