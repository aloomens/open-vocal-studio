cmake_minimum_required(VERSION 3.22)

project(VocalStudio VERSION 1.3.2 LANGUAGES C CXX)

# Product name shown to users (files, window, DAW). The CMake target, the plugin codes (Vcst / Vcsp)
# and the saved-state tag must never change, so the VST3 IDs stay the same and old projects load.
set(VS_PRODUCT_NAME "Open Vocal Studio")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_subdirectory(external/JUCE)

add_compile_definitions(VS_VERSION="${PROJECT_VERSION}" VS_PRODUCT_NAME="${VS_PRODUCT_NAME}")

# ---- ONNX Runtime (MIT): only for the DfnCheck developer test, as the reference that the plugin's
# own streaming DeepFilterNet3 engine (Source/DfnEngine.h) is checked against ----
set(ORT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/onnxruntime")
set(ORT_DLL "${ORT_DIR}/lib/onnxruntime.dll")

# DeepFilterNet3 model (MIT / Apache-2.0), embedded in the plugin binary
juce_add_binary_data(VocalStudioModels
    NAMESPACE BinaryData
    HEADER_NAME BinaryData.h
    SOURCES
        external/deepfilternet3/enc.onnx
        external/deepfilternet3/erb_dec.onnx
        external/deepfilternet3/df_dec.onnx)

# Windows: VST3 + app. Linux also builds LV2 (for Ardour and other LV2 hosts).
# The LV2 URI identifies the plugin in saved LV2 sessions: never change it once a Linux build is out.
set(VS_FORMATS VST3 Standalone)
set(VS_COMPANY_EMAIL "openstudio.plugins@gmail.com")
if(UNIX AND NOT APPLE)
    list(APPEND VS_FORMATS LV2)
    set(VS_COMPANY_EMAIL "mailto:openstudio.plugins@gmail.com")   # LV2 writes it as a URI (foaf:mbox)
endif()

juce_add_plugin(VocalStudio
    COMPANY_NAME "${VS_PRODUCT_NAME}"
    COMPANY_WEBSITE "https://aloomens.github.io/open-vocal-studio/"
    COMPANY_EMAIL "${VS_COMPANY_EMAIL}"
    PRODUCT_NAME "${VS_PRODUCT_NAME}"
    VERSION ${PROJECT_VERSION}
    PLUGIN_MANUFACTURER_CODE Vcst
    PLUGIN_CODE Vcsp
    FORMATS ${VS_FORMATS}
    LV2URI "https://aloomens.github.io/open-vocal-studio/"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon.png"
    BUNDLE_ID com.vocalstudio.vocalstudio
    VST3_CATEGORIES Fx Restoration Pitch Shift Dynamics Reverb)

# JUCE writes the Windows version resource once and doesn't notice a new VERSION; refresh it on every configure
file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/VocalStudio_artefacts/JuceLibraryCode/VocalStudio_resources.rc")

set(VS_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/DeepFilter.cpp)

target_sources(VocalStudio PRIVATE ${VS_SOURCES})

# On Linux there is no Windows Media or Core Audio to read MP3 files in the standalone file player:
# JUCE's own MP3 decoder does it there (part of JUCE, under the same AGPLv3).
set(VS_DEFS JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0
    $<$<PLATFORM_ID:Linux>:JUCE_USE_MP3AUDIOFORMAT=1>)
target_compile_definitions(VocalStudio PUBLIC ${VS_DEFS})

# ---- ASIO for the standalone app: Steinberg's ASIO SDK in external/asiosdk, used under GPLv3 ----
set(ASIO_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/asiosdk" CACHE PATH "Folder containing the ASIO SDK (with common/iasiodrv.h)")
if(WIN32 AND EXISTS "${ASIO_SDK_DIR}/common/iasiodrv.h")
    message(STATUS "ASIO SDK found at ${ASIO_SDK_DIR} - standalone app will support ASIO")
    target_compile_definitions(VocalStudio PUBLIC JUCE_ASIO=1)
    target_include_directories(VocalStudio PUBLIC "${ASIO_SDK_DIR}/common")
endif()

target_link_libraries(VocalStudio
    PRIVATE
        VocalStudioModels
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)


# ---- Tests: behaviour + THE_LAB-style measurements (compiles the plugin sources into a console app) ----
option(VS_BUILD_TESTS "Build the test programs" ON)
if(VS_BUILD_TESTS)
    juce_add_console_app(VocalTest PRODUCT_NAME "VocalTest")
    target_sources(VocalTest PRIVATE Tests/VocalTest.cpp ${VS_SOURCES})
    target_compile_definitions(VocalTest PRIVATE ${VS_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1)
    if(MSVC)
        target_link_options(VocalTest PRIVATE "/STACK:8388608")
    endif()
    target_link_libraries(VocalTest
        PRIVATE VocalStudioModels juce::juce_audio_utils juce::juce_dsp juce::juce_audio_formats
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

    # loads the built plugin like a DAW does (VST3; on Linux also LV2) and runs audio through it
    juce_add_console_app(PluginHostTest PRODUCT_NAME "PluginHostTest")
    target_sources(PluginHostTest PRIVATE Tests/HostTest.cpp)
    target_compile_definitions(PluginHostTest PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        $<$<PLATFORM_ID:Linux>:JUCE_PLUGINHOST_LV2=1>)
    target_link_libraries(PluginHostTest
        PRIVATE juce::juce_audio_processors juce::juce_audio_formats juce::juce_gui_basics juce::juce_dsp juce::juce_audio_devices
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endif()

# developer check of the neural denoiser alone (model I/O, chunked vs one-pass output, speed). It needs
# ONNX Runtime in external/onnxruntime (in the source zip; not in the git repository) and loads its
# Windows DLL: built on Windows only, and skipped without it.
if(VS_BUILD_TESTS AND WIN32 AND NOT EXISTS "${ORT_DLL}")
    message(STATUS "external/onnxruntime not found: DfnCheck (developer test against ONNX Runtime) is not built")
endif()
if(VS_BUILD_TESTS AND WIN32 AND EXISTS "${ORT_DLL}")
    juce_add_console_app(DfnCheck PRODUCT_NAME "DfnCheck")
    target_sources(DfnCheck PRIVATE Tests/DfnCheck.cpp Tests/OrtReference.cpp Source/DeepFilter.cpp)
    target_compile_definitions(DfnCheck PRIVATE ${VS_DEFS})
    target_include_directories(DfnCheck PRIVATE "${ORT_DIR}/include")
    target_link_libraries(DfnCheck
        PRIVATE VocalStudioModels juce::juce_audio_utils juce::juce_dsp juce::juce_audio_formats
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    add_custom_command(TARGET DfnCheck POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ORT_DLL}" "$<TARGET_FILE_DIR:DfnCheck>/vs_onnxruntime.dll")
endif()

if(VS_BUILD_TESTS)
    juce_add_console_app(PitchCheck PRODUCT_NAME "PitchCheck")
    target_sources(PitchCheck PRIVATE Tests/PitchCheck.cpp)
    target_compile_definitions(PitchCheck PRIVATE ${VS_DEFS})
    target_link_libraries(PitchCheck
        PRIVATE juce::juce_audio_utils juce::juce_dsp juce::juce_audio_formats
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endif()
