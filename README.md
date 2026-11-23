# wav-loader

A simple C++17 library to load RIFF WAVE files. Only PCM files are
supported, at all standard integer sizes (unsigned 8 bit, signed 16/24/32
bit, including sub-container valid-bits layouts such as 12/16 bit) as well
as 32 bit and 64 bit IEEE float, with or without
`WAVE_FORMAT_EXTENSIBLE`. Compressed formats (A-law, mu-law, GSM, ...) are
reported as a fatal `WavError::UnsupportedFormat`.

> This is a building block for my personal AI-driven DSP experiments. The only reason it's publicly available on GitHub is so that agents running in heavily constrained containers can easily access it without credentials (don't let agents run loose on your system with full disk access, people!) I don't claim this is well designed or fit for production use, but feel free to use it as you wish in your projects.

## Files

- `wav-loader.h` - public API (header)
- `wav-loader.cpp` - implementation

There are no configuration options and no third-party dependencies.

## API (`wav::WavFile`)

A single class loads WAVE data from a file, a stream or a byte container,
keeps it in its native format and returns the samples converted to any
supported data type through the `Get*()` functions.

### Loading

- `LoadFile(const std::string &path, uint64_t max_frames = 0)`
- `LoadStream(std::istream &stream, uint64_t max_frames = 0)`
- `LoadData(const std::vector<std::byte> &data, uint64_t max_frames = 0)`
  (data is copied internally)
- `LoadData(std::span<const std::byte> data, uint64_t max_frames = 0)`
  (C++20 builds only, also copied internally)

On memory-constrained systems, pass a non-zero `max_frames`: a file
containing more frames than allowed fails to load with
`WavError::TooManyFrames` instead of allocating memory for the whole
file. The default of 0 means no limit.

### Queries

- `SampleRate()`, `ChannelCount()`, `FrameCount()`, `DataType()`
- Error state: `HasError()`, `ErrorCode()` (`wav::WavError`),
  `ErrorString()` (hard-coded string), `IsFatal()` (distinguishes errors
  that completely prevent loading, such as `TooManyFrames` or
  `UnsupportedFormat`, from partial loading errors such as `DataTruncated`,
  `TrailingJunk` or `OversizedRiffChunk`).

### Getting the samples

`GetUnsigned8()`, `GetSigned16()`, `GetSigned24()`, `GetSigned32()`,
`GetFloat32()`, `GetFloat64()` return a
`std::vector<std::vector<T>>` with one vector per channel. If the
requested type differs from the native one, the samples are converted:

- float to int: the float range [-1, 1] maps to
  [-(2^(N-1) - 1), 2^(N-1) - 1] for an N-bit integer destination (e.g.
  [-127, 127] for int8_t, [-32767, 32767] for int16_t), so a full-scale
  signal is never flattened. Samples are auto-scaled by the highest
  finite absolute value in the file, guaranteeing no clipping.
  Non-finite samples (NaN, Infinity) are converted to zero (plus at most
  one LSB of dither) instead of producing undefined behavior. The getters
  that request the native float type return non-finite samples unchanged.
- int to int of lower depth: exact rescale with triangular (TPDF) dither
  at the destination LSB.
- int to int of greater depth, and float to float: exact, lossless
  conversion (no dither).

### Error handling and memory management

- The `Load*()` functions return true only when the file loaded with no
  error at all; on partial errors the loaded data still remains available.
- On a *fatal* error the getter functions return zero-sized vectors and
  the query functions return 0. On a *partial* error (e.g. trailing junk,
  a truncated data chunk, an oversized RIFF size) the data that was
  successfully read remains available through the query and getter
  functions.
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

## Building

```sh
c++ -std=c++17 -c wav-loader.cpp -I.
```

With C++20, a `std::span<const std::byte>` overload of `LoadData()` is
additionally available. There is nothing else to configure.

## AI-agent skill

An [OpenCode](https://opencode.ai) skill describing the library for
LLM coding agents is included at `skills/wav-loader/SKILL.md`. If you use
OpenCode, copy that directory into your project's `.opencode/skills/`
directory (or into `~/.config/opencode/skills/`); users of other coding
agents can copy `SKILL.md` to wherever their agent looks for skills or
instructions.

## Usage example

```cpp
#include "wav-loader.h"

wav::WavFile file;
if (file.LoadFile("audio.wav", 1000000)) {  // at most 1M frames
  std::vector<std::vector<float>> audio = file.GetFloat32();
  // audio[c][i] is sample i of channel c
  file.Clear(); // zero and free the file data, we have what we need
}
```

## License

This library was entirely generated by Z.ai's GLM-5.3 and is dedicated to
the public domain (with a CC0 1.0 fallback for jurisdictions that do not
recognize public domain dedications). See `LICENSE.md` for details.
