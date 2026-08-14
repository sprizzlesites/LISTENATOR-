# LISTENATOR

**Sprizzle** · An automatic vocal mixing suite as a VST3 plugin.

Feed it a dry vocal. Press **LISTEN**. It profiles the take, works out what's
wrong with it, and builds a corrective chain tuned to that specific voice —
then adds a modern vocal effects rack timed to your project.

Built with JUCE 9. Cross-compiled to Windows x64 from Linux with llvm-mingw.

---

## How it works

### The LISTEN pass

Nothing is a preset. A 15-second capture runs on a background thread and every
setting downstream is derived from what it measures:

| Measured | Method | Drives |
|---|---|---|
| LTAS | Welch-averaged 4096-pt FFT → 1/3-octave bands | Tone match, resonance detection |
| Noise floor | Minimum statistics (5th-percentile frame energy) | Gate threshold, de-noise depth |
| Loudness | ITU-R BS.1770 K-weighting, gated | Compressor thresholds, makeup |
| Crest factor | Peak − RMS | Compressor attack/release |
| F0 + intonation | YIN, 2048-pt frames | High-pass point, autotune retune speed |
| Formants F1–F3 | Cepstrally-smoothed spectral envelope | Voice characterisation |
| Sibilance | Spectral peak of the top-15% HF-ratio frames | De-esser band and threshold |
| Resonances | Peaks above a 1-octave log-frequency envelope | Surgical notches |
| RT60 | Decay-slope fit over note offsets | De-verb depth, gate release |

The static decisions get locked in. The inherently dynamic stages — resonance
suppression, de-esser, de-verb — keep adapting per block.

### The corrective half (CLEAN-INATOR)

Ordered the way the mixing literature agrees on. De-essing sits *after*
compression (compression amplifies sibilance) and *before* any additive HF:

```
HPF → de-noise → de-verb → gate → surgical EQ → resonance suppression
    → compression (2-stage) → de-ess → tone match → limiter
```

The high-pass goes below the singer's own lowest sung note (5th percentile of
the pitch track), not a fixed 80 Hz. The de-ess band is placed on the peak of
this singer's actual sibilance — male voices typically centre 5–6 kHz and
female 7–8 kHz, so a fixed band would be wrong about half the time.
Compression is two stages: a slow leveller sized by loudness range, then fast
peak control, both with attack and release derived from crest factor.

De-noise and de-verb scale their aggression to measured severity — near-zero on
a clean source, pushing hard only when the recording genuinely needs rescuing.

### The effects half (FX-INATOR)

```
autotune → doubler → saturation → air exciter → character → width
        → ducked delay → ducked reverb
```

Delay is tempo-locked from the host playhead (dotted-eighth left, quarter
right); reverb is sized to decay within two beats. Both sends duck under the
dry vocal so effects bloom in the gaps instead of washing out words. Autotune
uses TD-PSOLA, which preserves formants by construction, with retune speed
chosen from how unstable the performance actually was.

VST3 does not report song key, so the target key and scale are set manually.

### Controls

Two big switches bypass each half independently. Every module also has its own
bypass. The knobs under each rack *scale* the auto-derived values rather than
replacing them, so the plugin stays automatic but you can nudge it.

On a fresh instance audio passes through untouched until you press LISTEN.

---

## Building

### Windows VST3 (cross-compiled from Linux)

Needs [llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases) (a UCRT
build). Plain GCC-mingw does **not** work — see the notes below.

```bash
cmake -B build-win -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-llvm-mingw.cmake \
      -DCMAKE_BUILD_TYPE=Release .
cmake --build build-win
```

Output: `build-win/LISTENATOR_artefacts/Release/VST3/LISTENATOR.vst3`

### UI renderer

Screenshots the real editor headlessly, no DAW needed:

```bash
cmake -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DLISTENATOR_BUILD_UIRENDER=ON .
cmake --build build-linux --target UIRender
./build-linux/UIRender_artefacts/Release/LISTENATOR-UIRender ui-shots --analysed
```

### Verifying the DSP

`tools/dsptest.cpp` measures the processed audio against what the analysis
claimed it would do — 27 assertions covering cold-start transparency, analysis
accuracy against a known synthetic source, the applied EQ curve versus the
intended one, compression behaviour, band-selectivity of the de-esser, PSOLA
level preservation, trim-knob effect, bypass transparency and numerical
stability.

```bash
cmake -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DLISTENATOR_BUILD_TESTS=ON .
cmake --build build-linux --target DspTest
./build-linux/DspTest_artefacts/Release/DspTest
```

It exists because the first version of this chain had several defects that
listening alone would not have localised — an overlap-add writing behind its
read pointer, a de-ess threshold on the wrong magnitude scale, a compressor
detector mismatched to its threshold, and 31 EQ filters stacking several dB
past their target. Each is now a check.

### Verifying a build

`tools/vst3hosttest.cpp` is a minimal VST3 host: it loads the DLL, enumerates
the factory, instantiates the effect, runs audio through it and checks the
output is finite, non-silent and sanely levelled. Exit code 0 means the plugin
works.

```bash
SDK=JUCE/modules/juce_audio_processors_headless/format_types/VST3_SDK
x86_64-w64-mingw32-clang++ -std=c++17 -static -O1 -DRELEASE=1 -I$SDK \
    tools/vst3hosttest.cpp \
    $SDK/pluginterfaces/base/funknown.cpp \
    $SDK/pluginterfaces/base/coreiids.cpp \
    $SDK/public.sdk/source/vst/vstinitiids.cpp \
    -o vst3hosttest.exe -lole32 -luuid

wine64 vst3hosttest.exe 'Z:\path\to\LISTENATOR.vst3'
```

Note that `pluginval` is not usable here: its scan phase spawns a subprocess,
which Wine handles unreliably, so it times out before reaching the plugin. The
harness above drives the plugin directly instead.

---

## Cross-compilation notes

JUCE has no official MinGW support and mingw-w64's Windows SDK headers have
gaps. Everything needed to work around that is in `cmake/`:

- **JUCE 9 hard-refuses MinGW** with `#error "MinGW is not supported"`, and
  only defines `JUCE_64BIT` under `_MSC_VER` — without it `pointer_sized_int`
  silently becomes 32-bit. Patched in `cmake/patch-juce.cmake`.
- **mingw's UIAutomation headers omit the `CaretPosition` enum**, and
  `d2d1effects.h` omits the saturation effect's property enum. Both patched.
- **`IDWriteFactory4` hides the 6-argument `CreateCustomRenderingParams`**
  overload that MSVC's SDK keeps visible. Patched with a cast.
- **`NTDDI_VERSION` must be ≥ `NTDDI_WIN10_NI`** or `d2d1_3.h` hides
  `ID2D1DeviceContext3`; `_WIN32_WINNT` must be ≥ `0x0A00` for
  `GetDpiForWindow`.
- **A VST3 is a CMake MODULE library**, so `-static` has to go in
  `CMAKE_MODULE_LINKER_FLAGS` — `SHARED_LINKER_FLAGS` never reaches the plugin's
  link line. Without it the DLL imports `libc++.dll` and `libunwind.dll`, which
  no end user has.
- **`juce_add_plugin` builds the VST3 as a separate target**, so Windows system
  libs must be linked `PUBLIC` to propagate to it.
- **`VST3_AUTO_MANIFEST FALSE`** is required: the manifest helper is a host tool
  and cross-compiles into a Windows `.exe` that can't run on the build machine.
- **A few JUCE call sites instantiate `__uuidof` with the `ComSmartPtr`
  wrapper** rather than the interface. `Source/MinGWComShims.cpp` supplies the
  missing specialisations without further patching JUCE.

The force-included `cmake/mingw_compat.h` must only ever pull in std headers —
including `<windows.h>` there would precede JUCE's `UNICODE` setup and break
every `TCHAR`-derived type in the build.

---

## Licence and attribution

Built on **JUCE 9**, used here under the **AGPLv3**. Distributing this plugin
in binary form carries the AGPL's source-disclosure obligation; a commercial
JUCE licence lifts it.

`Source/UI/DoofArt.cpp` is deliberately the only file containing the
Doofenshmirtz / Phineas and Ferb homage — the caricature, the DEI logo and the
quote bank. Those elements are Disney-owned intellectual property and are
present here for personal, non-distributed use. Replacing that single
translation unit with original artwork makes the plugin distributable without
touching anything else.
