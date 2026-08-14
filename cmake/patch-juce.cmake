# Patches JUCE 9 so it can be cross-compiled for Windows with llvm-mingw.
#
# JUCE has no official MinGW support and mingw-w64's Windows SDK headers are
# incomplete in three specific places. Each patch below is idempotent (guarded
# by a marker string) so re-running configure is safe.
#
#   1. juce_TargetPlatform.h  - hard `#error` refusing to build under MinGW,
#                               and JUCE_64BIT only ever set for MSVC, which
#                               silently makes pointer_sized_int 32-bit.
#   2. juce_UIATextProvider   - mingw's uiautomation headers omit the
#                               CaretPosition enum entirely.
#   3. juce_Direct2DImage     - mingw's d2d1effects.h omits the saturation
#                               effect's property enum.
#   4. juce_Direct2DGraphics  - mingw's IDWriteFactory4 hides the 6-argument
#                               CreateCustomRenderingParams overload (MSVC's
#                               SDK keeps it visible via a `using` decl).

function(_ljp_patch_file path marker match replace)
    if(NOT EXISTS "${path}")
        message(WARNING "patch-juce: missing ${path}")
        return()
    endif()
    file(READ "${path}" _content)
    string(FIND "${_content}" "${marker}" _found)
    if(NOT _found EQUAL -1)
        return()  # already patched
    endif()
    string(REPLACE "${match}" "${replace}" _patched "${_content}")
    if("${_patched}" STREQUAL "${_content}")
        message(WARNING "patch-juce: pattern not found in ${path}")
        return()
    endif()
    file(WRITE "${path}" "${_patched}")
    message(STATUS "patch-juce: patched ${path}")
endfunction()

function(listenator_patch_juce JUCE_DIR)
    if(NOT JUCE_DIR)
        set(JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/JUCE")
    endif()

    # 1. Allow MinGW, and set JUCE_64BIT for it.
    _ljp_patch_file(
        "${JUCE_DIR}/modules/juce_core/system/juce_TargetPlatform.h"
        "LISTENATOR_MINGW_OK"
        "  #ifdef __MINGW32__\n    #error \"MinGW is not supported. Please use an alternative compiler.\"\n  #endif"
        "  // LISTENATOR_MINGW_OK: llvm-mingw cross-compilation is supported here.\n  #ifdef __MINGW32__\n    #if defined (_WIN64) && ! defined (JUCE_64BIT)\n      #define JUCE_64BIT 1\n    #endif\n  #endif")

    # 2. CaretPosition enum missing from mingw's uiautomation headers.
    _ljp_patch_file(
        "${JUCE_DIR}/modules/juce_gui_basics/native/accessibility/juce_UIATextProvider_windows.h"
        "LISTENATOR_CARETPOSITION"
        "namespace juce\n{"
        "// LISTENATOR_CARETPOSITION: absent from mingw-w64's UIAutomation headers.\n#if defined (__MINGW32__)\ntypedef enum CaretPosition\n{\n    CaretPosition_Unknown         = 0,\n    CaretPosition_EndOfLine       = 1,\n    CaretPosition_BeginningOfLine = 2\n} CaretPosition;\n#endif\n\nnamespace juce\n{")

    # 3. D2D saturation effect property enum missing from mingw's d2d1effects.h.
    _ljp_patch_file(
        "${JUCE_DIR}/modules/juce_graphics/native/juce_Direct2DImage_windows.cpp"
        "LISTENATOR_D2D_SATURATION"
        "effect->SetValue (D2D1_SATURATION_PROP_SATURATION, 0.0f);"
        "// LISTENATOR_D2D_SATURATION: enum absent from mingw's d2d1effects.h (SDK value is 0).\n       #if defined (__MINGW32__) && ! defined (D2D1_SATURATION_PROP_SATURATION)\n        #define D2D1_SATURATION_PROP_SATURATION 0\n       #endif\n        effect->SetValue (D2D1_SATURATION_PROP_SATURATION, 0.0f);")

    # 4. mingw's IDWriteFactory4 hides the base 6-arg overload; cast to select it.
    _ljp_patch_file(
        "${JUCE_DIR}/modules/juce_graphics/native/juce_Direct2DGraphicsContext_windows.cpp"
        "LISTENATOR_DWRITE_OVERLOAD"
        "if (FAILED (factory->CreateCustomRenderingParams (oldParams->GetGamma(),"
        "// LISTENATOR_DWRITE_OVERLOAD: mingw's derived factory hides the 6-arg overload.\n       #if defined (__MINGW32__)\n        #define JUCE_DWRITE_FACTORY_BASE ((IDWriteFactory*) factory)\n       #else\n        #define JUCE_DWRITE_FACTORY_BASE factory\n       #endif\n        if (FAILED (JUCE_DWRITE_FACTORY_BASE->CreateCustomRenderingParams (oldParams->GetGamma(),")
endfunction()
