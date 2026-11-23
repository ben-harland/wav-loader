---
name: wav-loader
description: How to load and convert RIFF WAVE (PCM) audio files in a C++ project using the wav-loader library.
---

# wav-loader

Use the `wav-loader` library whenever you need to read RIFF WAVE files in
a C++ project. It supports PCM only: unsigned 8 bit, signed 16/24/32 bit
(and sub-container valid-bits layouts such as 12-bit-in-16-bit) and 32/64
bit IEEE float, including `WAVE_FORMAT_EXTENSIBLE` files. Compressed
formats (A-law, µ-law, GSM, etc.) are reported as a fatal
`wav::WavError::UnsupportedFormat`.

## Getting the library

Download it from GitHub:

```sh
git clone https://github.com/ben-harland/wav-loader.git
```

The library consists of exactly two files: `wav-loader.h` (the public
API) and `wav-loader.cpp` (the implementation). Either build them in
place, or copy both files into your own source tree — that is all there
is to installation.

Build with C++17 (a C++20 build additionally enables a
`std::span<const std::byte>` overload):

```sh
c++ -std=c++17 -c wav-loader.cpp -I<directory containing wav-loader.h>
```

Add the directory containing `wav-loader.h` to the include path of any
translation unit that uses the library. There are no dependencies and no
configuration options. The whole API is the single class `wav::WavFile`.

Important constraints:

- A single `WavFile` instance is **not thread-safe**. The getter
  functions look `const` but mutate internal dither state, so concurrent
  calls on the same instance are a data race. Use one instance per thread
  or external synchronization.
- On memory-constrained systems, pass a non-zero `max_frames` argument to
  any `Load*()` call; files with more frames fail with
  `WavError::TooManyFrames` instead of allocating for the whole file.
- Integer getters convert non-finite float samples (NaN/Inf) to zero;
  native float getters return them unchanged.

## Loading

```cpp
#include "wav-loader.h"

wav::WavFile file;
if (!file.LoadFile("audio.wav", 1000000)) {  // max_frames: 0 = unlimited
  // file.ErrorString() explains what went wrong
}
```

`LoadFile`, `LoadStream(std::istream&, max_frames)` and `LoadData` (from a
`const std::vector<std::byte>&`, or a `std::span<const std::byte>` with
C++20) are the ways to feed data in. The data is kept in its native
format.

## Querying

```cpp
uint32_t rate = file.SampleRate();     // Hz (0 on fatal error)
uint16_t channels = file.ChannelCount();
uint64_t frames = file.FrameCount();
wav::WavDataType type = file.DataType();

// Error state
if (file.HasError()) {
  wav::WavError code = file.ErrorCode();   // enum
  const char *text = file.ErrorString();   // hard-coded string
  bool fatal = file.IsFatal();             // true = nothing could be loaded
}
```

## Getting the samples

Each getter returns a `std::vector<std::vector<T>>` with one vector per
channel; if the requested type differs from the native one, samples are
converted (float range [-1, 1] maps to [-(2^(N-1) - 1), 2^(N-1) - 1] for
an N-bit integer destination, auto-scaled by the highest *finite* absolute
value so nothing is clipped; non-finite samples become zero; integer
down-conversions get TPDF dither; up-conversions and float-to-float are
exact):

```cpp
std::vector<std::vector<float>> audio = file.GetFloat32();
// also: GetUnsigned8(), GetSigned16(), GetSigned24(), GetSigned32(),
//       GetFloat64()
```

The `Load*()` functions return true only when the file loaded with no
error at all. On a *fatal* error, getters return zero-sized vectors and
queries return 0. On a *partial* error (e.g. trailing junk after the RIFF
chunk, a truncated data chunk, an oversized RIFF size) the data that was
successfully read remains available through the queries and getters —
check `IsFatal()` to tell the two apart, and `ErrorCode()`/`ErrorString()`
for the specifics.

## Freeing memory

On memory-constrained platforms, call `Clear()` once you have retrieved
the samples in the format you need. It zeroes the internal copy of the
file data (so confidential audio does not linger in freed heap memory),
frees it, and resets the object to an empty, error-free state that can be
reused for another `Load...()` call.
