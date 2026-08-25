cmake_minimum_required(VERSION 3.22)

project(VoiceMorph VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(VM_ENABLE_ONNX "Build the neural voice-conversion stage against ONNX Runtime" OFF)
option(VM_COPY_PLUGIN "Copy the built VST3 into the system plugin folder" ON)
set(VM_ONNXRUNTIME_ROOT "" CACHE PATH "Root of an extracted onnxruntime release (contains include/ and lib/)")

# ---------------------------------------------------------------------------
# JUCE
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    # 8.0.15 is the last of the 8.x line and understands current MSVC
    # toolchains. JUCE 9 exists but has breaking API changes.
    GIT_TAG        8.0.15
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

# ---------------------------------------------------------------------------
# Plugin target
# ---------------------------------------------------------------------------
juce_add_plugin(VoiceMorph
    COMPANY_NAME            "IndieAudio"
    BUNDLE_ID               "com.indieaudio.voicemorph"
    IS_SYNTH                FALSE
    NEEDS_MIDI_INPUT        FALSE
    NEEDS_MIDI_OUTPUT       FALSE
    IS_MIDI_EFFECT          FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD ${VM_COPY_PLUGIN}
    PLUGIN_MANUFACTURER_CODE Idau
    PLUGIN_CODE              Vmr1
    FORMATS                  VST3 Standalone
    PRODUCT_NAME             "VoiceMorph")

target_sources(VoiceMorph PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/dsp/PhaseVocoderEngine.cpp
    Source/ai/NeuralVoiceConverter.cpp)

target_include_directories(VoiceMorph PRIVATE Source)

target_compile_definitions(VoiceMorph PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(VoiceMorph PRIVATE
    juce::juce_audio_devices
    juce::juce_audio_utils
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Optional: ONNX Runtime for the neural stage
# ---------------------------------------------------------------------------
if(VM_ENABLE_ONNX)
    if(NOT VM_ONNXRUNTIME_ROOT)
        message(FATAL_ERROR "VM_ENABLE_ONNX is ON but VM_ONNXRUNTIME_ROOT is not set.")
    endif()

    find_library(ORT_LIB onnxruntime PATHS "${VM_ONNXRUNTIME_ROOT}/lib" REQUIRED NO_DEFAULT_PATH)

    target_compile_definitions(VoiceMorph PRIVATE VM_ENABLE_ONNX=1)
    target_include_directories(VoiceMorph PRIVATE "${VM_ONNXRUNTIME_ROOT}/include")
    target_link_libraries(VoiceMorph PRIVATE "${ORT_LIB}")

    message(STATUS "VoiceMorph: neural stage enabled, linking ${ORT_LIB}")
else()
    message(STATUS "VoiceMorph: neural stage disabled (DSP-only build)")
endif()
