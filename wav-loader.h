#ifndef WAV_LOADER_H
#define WAV_LOADER_H

#include <cstddef>
#include <cstdint>
#include <istream>
#include <string>
#include <vector>

#if defined(__cplusplus) && __cplusplus >= 202002L
#include <span>
#define WAV_LOADER_HAVE_SPAN 1
#endif

namespace wav {

/// Format of the sample data in a WAVE file.
enum class WavDataType {
  Unknown,   ///< Unknown or unsupported data format.
  Unsigned8, ///< Unsigned 8 bit integer data (the standard WAVE 8 bit format).
  Signed16,  ///< Signed 16 bit integer data.
  Signed24,  ///< Signed 24 bit integer data.
  Signed32,  ///< Signed 32 bit integer data.
  Float32,   ///< 32 bit IEEE float data.
  Float64,   ///< 64 bit IEEE float data.
};

/// Error codes reported by the loader.
///
/// Fatal errors completely prevent loading data. Partial errors mean that
/// the file is inconsistent, but the data that was successfully read
/// remains available through the query and getter functions.
enum class WavError {
  None = 0,              ///< No error.
  FileNotFound,          ///< (Fatal) The file could not be opened.
  ReadError,             ///< (Fatal) The stream could not be read.
  NotRiffFile,           ///< (Fatal) The data is not a RIFF file.
  NotWaveFile,           ///< (Fatal) The RIFF file is not a WAVE file.
  MissingFormatChunk,    ///< (Fatal) No "fmt " chunk found.
  InvalidFormatChunk,    ///< (Fatal) The "fmt " chunk is malformed.
  UnsupportedFormat,     ///< (Fatal) The sample format is not supported (only PCM is supported).
  MissingDataChunk,      ///< (Fatal) No "data" chunk found.
  EmptyDataChunk,        ///< (Fatal) The "data" chunk contains no samples.
  TooManyFrames,         ///< (Fatal) The file has more frames than the maximum allowed for loading.
  DataTruncated,         ///< (Partial) The file ends in the middle of the sample data.
  TrailingJunk,          ///< (Partial) There is extra data after the RIFF chunk.
  OversizedRiffChunk,    ///< (Partial) The declared RIFF chunk size is larger than the available data.
};

/// Basic WAVE file loader.
///
/// Loads RIFF WAVE (PCM) data from a file, a stream or a byte container,
/// keeps it in its native format and returns the samples converted to any
/// supported data type through the Get*() functions.
///
/// Supported formats: unsigned 8 bit, signed 16/24/32 bit (including
/// sub-container valid-bits layouts such as 12-bit-in-16-bit) and 32/64 bit
/// IEEE float, with or without WAVE_FORMAT_EXTENSIBLE. Compressed formats
/// (A-law, mu-law, GSM, ...) are reported as a fatal
/// WavError::UnsupportedFormat.
///
/// On a fatal loading error the getter functions return zero-sized vectors
/// and the query functions return 0. On a partial error (e.g. trailing
/// junk, a truncated data chunk) the data that was successfully read
/// remains available.
///
/// Memory-constrained systems: every Load*() function accepts an optional
/// max_frames argument. If the file contains more frames than allowed, the
/// load fails with WavError::TooManyFrames instead of allocating memory
/// for the whole file.
///
/// Non-finite float samples (NaN, Infinity) are returned unchanged by the
/// getters that request the native float type; the integer getters convert
/// them to zero instead of invoking undefined behavior.
///
/// Thread safety: a single WavFile instance is not thread-safe. The
/// getter functions look const but mutate internal dither state, so
/// concurrent calls on the same instance (or a load concurrent with a
/// getter) are a data race. Use one instance per thread, or protect a
/// shared instance with external synchronization.
class WavFile {
public:
  /// Loads WAVE data from a file. Returns true on success (no error at all).
  ///
  /// If an error occured, some data may still have been loaded.
  ///
  /// \param max_frames Maximum number of frames to accept; 0 (the default)
  ///        means no limit. A file with more frames fails to load with
  ///        WavError::TooManyFrames.
  bool LoadFile(const std::string &path, uint64_t max_frames = 0);

  /// Loads WAVE data from a stream. Returns true on success (no error at
  /// all). See LoadFile() for the max_frames parameter.
  bool LoadStream(std::istream &stream, uint64_t max_frames = 0);

  /// Loads WAVE data from a byte vector (copied internally).
  /// Returns true on success (no error at all).
  /// See LoadFile() for the max_frames parameter.
  bool LoadData(const std::vector<std::byte> &data, uint64_t max_frames = 0);

#if defined(WAV_LOADER_HAVE_SPAN)
  /// Loads WAVE data from a byte span (copied internally).
  /// Returns true on success (no error at all).
  /// See LoadFile() for the max_frames parameter.
  bool LoadData(std::span<const std::byte> data, uint64_t max_frames = 0);
#endif

  /// Frees the loaded WAVE data. Call this on memory-constrained platforms
  /// once the samples have been retrieved in the desired format with the
  /// Get*() functions. The internal buffer is zeroed before being released,
  /// so confidential audio content does not linger in freed heap memory.
  /// The object returns to an empty, error-free state and can be reused
  /// for another Load*() call.
  void Clear();

  /// Returns true if an error was encountered while loading.
  bool HasError() const {
    return error_ != WavError::None;
  }

  /// Returns the error code of the last load.
  WavError ErrorCode() const {
    return error_;
  }

  /// Returns the error string of the last load.
  const char *ErrorString() const;

  /// Returns true if the error completely prevented loading data.
  bool IsFatal() const {
    switch (error_) {
    case WavError::FileNotFound:
    case WavError::ReadError:
    case WavError::NotRiffFile:
    case WavError::NotWaveFile:
    case WavError::MissingFormatChunk:
    case WavError::InvalidFormatChunk:
    case WavError::UnsupportedFormat:
    case WavError::MissingDataChunk:
    case WavError::EmptyDataChunk:
    case WavError::TooManyFrames:
      return true;
    default:
      return false;
    }
  }

  /// Returns the sampling rate in Hz (0 on fatal error).
  uint32_t SampleRate() const {
    return IsFatal() ? 0 : sample_rate_;
  }

  /// Returns the number of channels (0 on fatal error).
  uint16_t ChannelCount() const {
    return IsFatal() ? 0 : channel_count_;
  }

  /// Returns the number of frames (0 on fatal error).
  uint64_t FrameCount() const {
    return IsFatal() ? 0 : frame_count_;
  }

  /// Returns the native data type of the file (Unknown on fatal error).
  WavDataType DataType() const {
    return IsFatal() ? WavDataType::Unknown : data_type_;
  }

  /// Returns the samples as unsigned 8 bit integers, one vector per
  /// channel (empty on fatal error).
  std::vector<std::vector<uint8_t>> GetUnsigned8();

  /// Returns the samples as signed 16 bit integers, one vector per
  /// channel (empty on fatal error).
  std::vector<std::vector<int16_t>> GetSigned16();

  /// Returns the samples as signed 32 bit integers holding 24 bit values,
  /// one vector per channel (empty on fatal error).
  std::vector<std::vector<int32_t>> GetSigned24();

  /// Returns the samples as signed 32 bit integers, one vector per
  /// channel (empty on fatal error).
  std::vector<std::vector<int32_t>> GetSigned32();

  /// Returns the samples as 32 bit floats, one vector per channel
  /// (empty on fatal error).
  std::vector<std::vector<float>> GetFloat32();

  /// Returns the samples as 64 bit floats, one vector per channel
  /// (empty on fatal error).
  std::vector<std::vector<double>> GetFloat64();

private:
  /// Resets the parse state members without touching the error state.
  void ResetParseState();

  /// Parses the internal buffer, filling the parse state members.
  /// Fails with WavError::TooManyFrames if max_frames is non-zero and the
  /// file contains more frames.
  bool ParseBuffer(uint64_t max_frames);

  /// Converts the loaded data to the given sample type, one vector per
  /// channel. Instantiated only for the supported destination types.
  template <typename T>
  std::vector<std::vector<T>> GetAs() const;

  /// Reads the sample at pos as a double, normalized to -1..1.
  double ReadSampleDouble(const std::byte *pos) const;

  /// Returns the highest absolute sample value in the data (float sources).
  double ScanMaxAbs() const;

  /// Generates a triangular probability density function noise value in [-1, 1).
  double DitherNoise() const;

  uint32_t sample_rate_ = 0;
  uint16_t channel_count_ = 0;
  uint64_t frame_count_ = 0;
  uint16_t container_bytes_ = 0;   ///< Bytes per sample in the file.
  unsigned shift_ = 0;             ///< Right shift to extract valid bits from the container.
  unsigned valid_bits_ = 0;        ///< Number of valid (significant) bits of the source samples.
  double full_scale_ = 1.0;        ///< Full scale of the valid bits (2^(valid bits - 1)).
  bool is_unsigned_ = false;       ///< True for unsigned 8 bit data.
  bool is_float_ = false;          ///< True for IEEE float data.
  WavDataType data_type_ = WavDataType::Unknown;
  const std::byte *data_begin_ = nullptr;
  const std::byte *data_end_ = nullptr;
  WavError error_ = WavError::None;
  mutable uint32_t prng_ = 0x12345678u;
  std::vector<std::byte> file_data_; ///< Owned copy of the file data.
};

} // namespace wav

#endif // WAV_LOADER_H
