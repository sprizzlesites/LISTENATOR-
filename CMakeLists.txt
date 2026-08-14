cmake_minimum_required(VERSION 3.22)
project(LISTENATOR VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# JUCE
# ---------------------------------------------------------------------------
# Prefer a JUCE checkout sitting next to us (fast iteration / offline builds),
# otherwise fetch the pinned tag.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
    add_subdirectory(JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        9.0.1
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# JUCE 9 refuses to build under MinGW and only sets JUCE_64BIT for MSVC, and
# mingw-w64's UIAutomation headers are missing the CaretPosition enum.
# patch-juce.cmake fixes all three, idempotently.
if(MINGW OR CMAKE_CXX_COMPILER MATCHES "mingw")
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch-juce.cmake)
    listenator_patch_juce("${JUCE_SOURCE_DIR}")
endif()

# ---------------------------------------------------------------------------
# Plugin target
# ---------------------------------------------------------------------------
juce_add_plugin(LISTENATOR
    COMPANY_NAME                "Sprizzle"
    COMPANY_WEBSITE             "https://sprizzle.io"
    BUNDLE_ID                   "io.sprizzle.listenator"
    PLUGIN_MANUFACTURER_CODE    Sprz
    PLUGIN_CODE                 Lstn
    FORMATS                     VST3
    PRODUCT_NAME                "LISTENATOR"
    DESCRIPTION                 "Automatic vocal mixing suite"
    VST3_CATEGORIES             Fx Dynamics EQ Pitch Shift
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD     FALSE
    # The manifest helper is built as a host tool; when cross-compiling it comes
    # out as a Windows .exe that can't run on the build machine.
    VST3_AUTO_MANIFEST          FALSE)

target_sources(LISTENATOR PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/MinGWComShims.cpp
    Source/Analysis/VocalAnalyzer.cpp
    Source/Analysis/PitchTracker.cpp
    Source/DSP/CleanupChain.cpp
    Source/DSP/EffectsChain.cpp
    Source/DSP/PitchCorrector.cpp
    Source/UI/DoofLookAndFeel.cpp
    Source/UI/DoofArt.cpp
    Source/UI/MachinePanel.cpp)

target_include_directories(LISTENATOR PRIVATE Source)

target_compile_definitions(LISTENATOR PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    # mingw's headers lack the NS_E_* Windows Media error codes, and a VST
    # has no use for WMA decoding.
    JUCE_USE_WINDOWS_MEDIA_FORMAT=0)

target_link_libraries(LISTENATOR
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags)

# mingw doesn't pull these in implicitly the way MSVC does. These must be
# PUBLIC: juce_add_plugin builds the VST3 as a separate target that links
# against the shared-code library, and PRIVATE deps don't propagate to it.
if(MINGW OR CMAKE_CXX_COMPILER MATCHES "mingw")
    target_link_libraries(LISTENATOR PUBLIC
        ole32 oleaut32 uuid dxguid dbghelp shcore dcomp dwmapi sapi
        dxgi d3d11 d2d1 dwrite windowscodecs winmm version imm32
        comctl32 comdlg32 shlwapi rpcrt4 gdi32 user32 shell32 advapi32
        ws2_32 wininet)
endif()

# ---------------------------------------------------------------------------
# Headless UI renderer — screenshots the real editor without a plugin host
# ---------------------------------------------------------------------------
if(LISTENATOR_BUILD_UIRENDER)
    juce_add_gui_app(UIRender PRODUCT_NAME "LISTENATOR-UIRender")
    target_sources(UIRender PRIVATE
        tools/UIRender.cpp
        Source/MinGWComShims.cpp
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/Analysis/VocalAnalyzer.cpp
        Source/Analysis/PitchTracker.cpp
        Source/DSP/CleanupChain.cpp
        Source/DSP/EffectsChain.cpp
        Source/DSP/PitchCorrector.cpp
        Source/UI/DoofLookAndFeel.cpp
        Source/UI/DoofArt.cpp
        Source/UI/MachinePanel.cpp)
    target_include_directories(UIRender PRIVATE Source)
    target_compile_definitions(UIRender PUBLIC
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        JUCE_DISPLAY_SPLASH_SCREEN=0 JUCE_REPORT_APP_USAGE=0
        JUCE_STANDALONE_APPLICATION=1
        JUCE_USE_WINDOWS_MEDIA_FORMAT=0
        # lets the tool pump the message loop so editor timers fire before capture
        JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(UIRender
        PRIVATE juce::juce_audio_utils juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags)
    if(MINGW OR CMAKE_CXX_COMPILER MATCHES "mingw")
        target_link_libraries(UIRender PUBLIC
            ole32 oleaut32 uuid dxguid dbghelp shcore dcomp dwmapi sapi ksuser mfplat
            dxgi d3d11 d2d1 dwrite windowscodecs winmm version imm32
            comctl32 comdlg32 shlwapi rpcrt4 gdi32 user32 shell32 advapi32
            ws2_32 wininet)
    endif()
endif()

# ---------------------------------------------------------------------------
# DSP verification harness — measures the processed audio against what the
# analysis said it would do. Native build only.
# ---------------------------------------------------------------------------
if(LISTENATOR_BUILD_TESTS)
    juce_add_console_app(DspTest PRODUCT_NAME "DspTest")
    target_sources(DspTest PRIVATE
        tools/dsptest.cpp
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/Analysis/VocalAnalyzer.cpp
        Source/Analysis/PitchTracker.cpp
        Source/DSP/CleanupChain.cpp
        Source/DSP/EffectsChain.cpp
        Source/DSP/PitchCorrector.cpp
        Source/UI/DoofLookAndFeel.cpp
        Source/UI/DoofArt.cpp
        Source/UI/MachinePanel.cpp)
    target_include_directories(DspTest PRIVATE Source)
    target_compile_definitions(DspTest PUBLIC
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        JUCE_DISPLAY_SPLASH_SCREEN=0 JUCE_REPORT_APP_USAGE=0
        JUCE_STANDALONE_APPLICATION=1 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(DspTest
        PRIVATE juce::juce_audio_utils juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags)
endif()

# ---------------------------------------------------------------------------
# Offline renderer — runs real audio files through the plugin for tuning
# ---------------------------------------------------------------------------
if(LISTENATOR_BUILD_TESTS)
    juce_add_console_app(Render PRODUCT_NAME "Render")
    target_sources(Render PRIVATE
        tools/render.cpp
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/Analysis/VocalAnalyzer.cpp
        Source/Analysis/PitchTracker.cpp
        Source/DSP/CleanupChain.cpp
        Source/DSP/EffectsChain.cpp
        Source/DSP/PitchCorrector.cpp
        Source/UI/DoofLookAndFeel.cpp
        Source/UI/DoofArt.cpp
        Source/UI/MachinePanel.cpp)
    target_include_directories(Render PRIVATE Source)
    target_compile_definitions(Render PUBLIC
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        JUCE_DISPLAY_SPLASH_SCREEN=0 JUCE_REPORT_APP_USAGE=0
        JUCE_STANDALONE_APPLICATION=1 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(Render
        PRIVATE juce::juce_audio_utils juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags)
endif()
