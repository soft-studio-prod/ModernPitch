cmake_minimum_required(VERSION 3.22)

project(ModernPitch VERSION 0.3.2)

if(NOT DEFINED JUCE_PATH)
    message(FATAL_ERROR "Please provide -DJUCE_PATH=C:/path/to/JUCE")
endif()

add_subdirectory("${JUCE_PATH}" JUCE)

# Public source repository, shown in the plugin's About panel (AGPLv3).
# Forks that distribute builds should point it at their own repository.
set(MODERNPITCH_SOURCE_URL "https://github.com/soft-studio-prod/ModernPitch"
    CACHE STRING "URL of the public source repository")

juce_add_plugin(ModernPitch
    COMPANY_NAME "Soft Studio"
    BUNDLE_ID "com.modernpitch.pitch"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    PLUGIN_MANUFACTURER_CODE Mpch
    PLUGIN_CODE Mp01
    FORMATS VST3
    PRODUCT_NAME "ModernPitch"
)

target_sources(ModernPitch PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/PluginEditor.cpp
    Source/PluginEditor.h
    Source/PsolaEngine.h
    Source/PsolaPitchDetector.h
    Source/RingModulator.h
    Source/ToneFilters.h
)

target_compile_features(ModernPitch PRIVATE cxx_std_17)

target_compile_definitions(ModernPitch PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    MODERNPITCH_VERSION=\"${PROJECT_VERSION}\"
    MODERNPITCH_SOURCE_URL=\"${MODERNPITCH_SOURCE_URL}\"
)

target_link_libraries(ModernPitch PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
)

juce_generate_juce_header(ModernPitch)

# Offline test bench (Tools/PsolaRender.cpp): -DMODERNPITCH_BUILD_TOOLS=ON
option(MODERNPITCH_BUILD_TOOLS "Build the offline PSOLA render tool" OFF)
if(MODERNPITCH_BUILD_TOOLS)
    juce_add_console_app(PsolaRender PRODUCT_NAME "PsolaRender")
    target_sources(PsolaRender PRIVATE Tools/PsolaRender.cpp)
    target_compile_features(PsolaRender PRIVATE cxx_std_17)
    target_compile_definitions(PsolaRender PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(PsolaRender PRIVATE juce::juce_audio_formats juce::juce_audio_basics)
    juce_generate_juce_header(PsolaRender)
endif()
