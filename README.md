# wav-loader

A simple C++17 library to load RIFF WAVE files.

Only PCM files are supported, at all standard integer sizes (unsigned 8 bit,
signed 16/24/32 bit, including sub-container valid-bits layouts such as 12/16
bit) as well as 32 bit and 64 bit IEEE float, with or without
`WAVE_FORMAT_EXTENSIBLE`. Compressed formats (A-law, mu-law, GSM, ...) are
reported as a fatal `WavError::UnsupportedFormat`. Any metadata in the file is
ignored; this library only loads sample data.

> This is a building block for my personal AI-driven DSP experiments. The only
> reason it's publicly available on GitHub is so that agents running in heavily
> constrained containers can easily access it without credentials (don't let
> agents run loose on your system with full disk access, people!) I don't claim
> this is well designed or fit for production use, but feel free to use it as
> you wish in your projects.

## Getting started

The library is a single header, `wav-loader.h`, containing both the API and
the implementation. There are no dependencies and nothing to link: just
include it wherever you need it, and compile your code as C++17 or later
(C++20 is recommended: it unlocks an extra `LoadData()` overload taking a
`std::span`).

```sh
c++ -std=c++20 -I<directory containing wav-loader.h> your-code.cpp
```

## Quick start

```cpp
#include "wav-loader.h"

wav::WavFile file;
if (file.LoadFile("audio.wav")) {
  std::vector<std::vector<float>> audio = file.GetFloat32();
  // audio[c][i] is sample i of channel c
  file.Clear(); // zero and free the file data, we have what we need
}
```

That is all a typical user needs. The rest of this document describes the
full API and the fine points of the conversion and error-handling behavior.

## API (`wav::WavFile`)

### Loading

- `LoadFile(const std::string &path, uint64_t max_frames = 0)` — load from
  a file.
- `LoadStream(std::istream &stream, uint64_t max_frames = 0)` — load from
  an already-open stream.
- `LoadData(const std::vector<std::byte> &data, uint64_t max_frames = 0)` —
  load from a byte vector (copied internally).
- `LoadData(std::span<const std::byte> data, uint64_t max_frames = 0)` —
  load from a byte span, also copied internally (C++20 builds only).

Each function returns true only when the file loaded with no error at all.
On memory-constrained systems, pass a non-zero `max_frames`: a file with
more frames fails to load with `WavError::TooManyFrames` instead of
allocating memory for the whole file. The default of 0 means no limit.

### Queries

- `SampleRate()` — sampling rate in Hz.
- `ChannelCount()` — number of channels.
- `FrameCount()` — number of frames (samples per channel).
- `DataType()` — the native sample format of the file (`wav::WavDataType`).
- `HasError()` — true if any error occurred during the last load.
- `ErrorCode()` — the `wav::WavError` code of the last load.
- `ErrorString()` — human-readable description of the last error
  (hard-coded string).
- `IsFatal()` — true if the error completely prevented loading data.
- `Clear()` — zero and free the loaded data; the object returns to an
  empty, error-free state and can be reused.

### Getting the samples

`GetUnsigned8()`, `GetSigned16()`, `GetSigned24()`, `GetSigned32()`,
`GetFloat32()`, `GetFloat64()` return a `std::vector<std::vector<T>>` with
one vector per channel, in the requested data type. If the requested type
differs from the native one, the samples are converted; the exact rules
are described in the bit-depth conversion section below.

## AI-agent skill

An [OpenCode](https://opencode.ai) skill describing the library for
LLM coding agents is included at `skills/wav-loader/SKILL.md`. If you use
OpenCode, copy that directory into your project's `.opencode/skills/`
directory (or into `~/.config/opencode/skills/`); users of other coding
agents can copy `SKILL.md` to wherever their agent looks for skills or
instructions.

## In depth

### Bit-depth conversions

- float to int: the library scans the data for the highest finite
  absolute sample value, `m`, and uses it as the full scale: the range
  `[-m, m]` maps to `[-(2^(N-1) - 1), 2^(N-1) - 1]` for an N-bit integer
  destination. For example, if the loudest sample in the file is -45.65,
  the range [-45.65, 45.65] maps to [-127, 127] when converting to 8 bit.
  This guarantees that no sample is ever clipped. (If the file contains
  no finite samples at all, a full scale of 1.0 is used.) Non-finite
  samples (NaN, Infinity) are converted to zero (plus at most one LSB of
  dither) instead of producing undefined behavior. The getters that
  request the native float type return non-finite samples unchanged.
- int to float: the full integer range is mapped to [-1, 1] exactly. The
  most negative integer value maps to -1; the most positive value maps to
  just under +1, since the integer range is not balanced. No dithering is
  applied.
- int to int of lower depth: exact rescale with triangular (TPDF) dither
  at the destination LSB.
- int to int of greater depth: exact power-of-two rescale, no dithering.
- float to float: samples are converted with a plain conversion, without
  dithering. In particular, converting double to float loses precision
  and there is no dithering to mask it.

### Error handling

- The `Load*()` functions return true only when the file loaded with no
  error at all; on partial errors the loaded data still remains available.
- On a *fatal* error the getter functions return zero-sized vectors and
  the query functions return 0. On a *partial* error (e.g. trailing junk,
  a truncated data chunk, an oversized RIFF size) the data that was
  successfully read remains available through the query and getter
  functions. Use `IsFatal()` to tell the two apart, and `ErrorCode()` /
  `ErrorString()` for the specifics.
- `Clear()` frees the loaded WAVE data. On memory-constrained platforms,
  call it once the samples have been retrieved in the desired format. The
  buffer is zeroed before being released, so confidential audio content
  does not linger in freed heap memory. The object returns to an empty,
  error-free state and can be reused for another `Load*()` call.

### Thread safety

A single `WavFile` instance is **not** thread-safe. The getter functions
look `const` but mutate internal dither state, so concurrent calls on the
same instance (or a load concurrent with a getter) are a data race. Use
one instance per thread, or protect a shared instance with external
synchronization.

## License

This library was entirely generated by Z.ai's GLM-5.3 and is dedicated to
the public domain (with a CC0 1.0 fallback for jurisdictions that do not
recognize public domain dedications). See `LICENSE.md` for details.
