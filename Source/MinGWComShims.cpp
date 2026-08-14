// Link-time shims for cross-compiling JUCE to Windows with llvm-mingw.
//
// A handful of JUCE call sites end up instantiating __uuidof() with the
// ComSmartPtr wrapper rather than the COM interface it wraps. MSVC resolves
// this through its own __uuidof intrinsic; mingw's __mingw_uuidof<T> has no
// specialisation for the wrapper, so the symbol goes unresolved at link time.
//
// Defining the specialisations here forwards them to the wrapped interface.
// This lives in our own code so the vendored JUCE tree stays closer to stock.

#if defined (__MINGW32__)

#include <windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>

namespace juce { template <class ComClass> class ComSmartPtr; }

template <>
const GUID& __mingw_uuidof<juce::ComSmartPtr<IDXGIDevice>>()
{
    return __mingw_uuidof<IDXGIDevice>();
}

template <>
const GUID& __mingw_uuidof<juce::ComSmartPtr<IDXGISurface>>()
{
    return __mingw_uuidof<IDXGISurface>();
}

template <>
const GUID& __mingw_uuidof<juce::ComSmartPtr<IDXGISurface1>>()
{
    return __mingw_uuidof<IDXGISurface1>();
}

#endif // __MINGW32__
