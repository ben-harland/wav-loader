#include "wav-loader.h"

#include <fstream>
#include <math.h>
#include <string.h>

namespace wav {

namespace {

uint16_t ReadU16(const std::byte *pos) {
  return static_cast<uint16_t>(static_cast<uint8_t>(pos[0]) |
                               (static_cast<uint8_t>(pos[1]) << 8));
}

uint32_t ReadU32(const std::byte *pos) {
  return static_cast<uint32_t>(static_cast<uint8_t>(pos[0])) |
         (static_cast<uint32_t>(static_cast<uint8_t>(pos[1])) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(pos[2])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(pos[3])) << 24);
}

uint64_t ReadU64(const std::byte *pos) {
  return static_cast<uint64_t>(ReadU32(pos)) |
         (static_cast<uint64_t>(ReadU32(pos + 4)) << 32);
}

bool IdEquals(const std::byte *pos, const char *id) {
  return memcmp(pos, id, 4) == 0;
}

/// Traits of the supported destination sample types.
template <typename T>
struct DestTraits;

template <> struct DestTraits<uint8_t> {
  static constexpr bool is_float = false;
  static constexpr bool is_unsigned = true;
  static constexpr double full_scale = 128.0;
  static constexpr unsigned bits = 8;
};
template <> struct DestTraits<int16_t> {
  static constexpr bool is_float = false;
  static constexpr bool is_unsigned = false;
  static constexpr double full_scale = 32768.0;
  static constexpr unsigned bits = 16;
};
template <> struct DestTraits<int32_t> {
  static constexpr bool is_float = false;
  static constexpr bool is_unsigned = false;
  static constexpr double full_scale = 2147483648.0;
  static constexpr unsigned bits = 32;
};
template <> struct DestTraits<float> {
  static constexpr bool is_float = true;
  static constexpr bool is_unsigned = false;
  static constexpr double full_scale = 1.0;
  static constexpr unsigned bits = 32;
};
template <> struct DestTraits<double> {
  static constexpr bool is_float = true;
  static constexpr bool is_unsigned = false;
  static constexpr double full_scale = 1.0;
  static constexpr unsigned bits = 64;
};

/// Converts a normalized value (-1..1) to the destination sample type.
///
/// When the source is float, [-1, 1] maps to
/// [-(2^(N-1) - 1), 2^(N-1) - 1] for an N-bit integer destination, so a
/// full-scale signal is never flattened. Integer sources keep the exact
/// power-of-two rescale (the negative extreme is representable anyway).
/// dither_noise is a TPDF noise value in [-1, 1) at the destination LSB,
/// or 0 when no dithering is needed.
template <typename T>
T DoubleToDest(double value, double dither_noise, bool source_float) {
  if constexpr (DestTraits<T>::is_float) {
    (void)DestTraits<T>::is_unsigned;
    (void)DestTraits<T>::full_scale;
    return static_cast<T>(value);
  }
  else {
    const double full_scale = DestTraits<T>::full_scale;
    const double limit = full_scale - 1.0;
    const double scale = source_float ? limit : full_scale;
    double scaled = value * scale + dither_noise;
    double rounded = floor(scaled + 0.5);
    if (rounded > limit) {
      rounded = limit;
    }
    else if (rounded < (source_float ? -limit : -full_scale)) {
      rounded = source_float ? -limit : -full_scale;
    }
    if constexpr (DestTraits<T>::is_unsigned) {
      return static_cast<T>(rounded + full_scale);
    }
    else {
      return static_cast<T>(rounded);
    }
  }
}

} // namespace

double WavFile::DitherNoise() const {
  prng_ = prng_ * 1664525u + 1013904223u;
  const double a = static_cast<double>(prng_ >> 8) / 16777216.0;
  prng_ = prng_ * 1664525u + 1013904223u;
  const double b = static_cast<double>(prng_ >> 8) / 16777216.0;
  return a + b - 1.0;
}

double WavFile::ReadSampleDouble(const std::byte *pos) const {
  if (is_unsigned_) {
    return (static_cast<double>(static_cast<uint8_t>(pos[0])) - 128.0) / 128.0;
  }
  if (is_float_) {
    // Reassemble the bit pattern in little-endian order first so the
    // decoded value does not depend on the host byte order.
    if (container_bytes_ == 8) {
      const uint64_t bits = ReadU64(pos);
      double value = 0.0;
      memcpy(&value, &bits, 8);
      return value;
    }
    const uint32_t bits = ReadU32(pos);
    float value = 0.0f;
    memcpy(&value, &bits, 4);
    return static_cast<double>(value);
  }
  int32_t raw = 0;
  memcpy(&raw, pos, container_bytes_);
  if (container_bytes_ < 4 && (raw & (1 << (container_bytes_ * 8 - 1))) != 0) {
    raw |= ~((1 << (container_bytes_ * 8)) - 1);
  }
  const int32_t valid = static_cast<int32_t>(raw) >> shift_;
  return static_cast<double>(valid) / full_scale_;
}

double WavFile::ScanMaxAbs() const {
  const size_t block = static_cast<size_t>(container_bytes_) * channel_count_;
  double max_abs = 0.0;
  for (uint64_t i = 0; i < frame_count_; ++i) {
    for (uint16_t c = 0; c < channel_count_; ++c) {
      const double value = fabs(ReadSampleDouble(data_begin_ + i * block + c * container_bytes_));
      // Non-finite samples (NaN, Inf) must not influence the scaling; they
      // are converted to zero by the getters.
      if (isfinite(value) && value > max_abs) {
        max_abs = value;
      }
    }
  }
  return max_abs;
}

void WavFile::ResetParseState() {
  sample_rate_ = 0;
  channel_count_ = 0;
  frame_count_ = 0;
  container_bytes_ = 0;
  shift_ = 0;
  valid_bits_ = 0;
  full_scale_ = 1.0;
  is_unsigned_ = false;
  is_float_ = false;
  data_type_ = WavDataType::Unknown;
  data_begin_ = nullptr;
  data_end_ = nullptr;
}

bool WavFile::ParseBuffer(uint64_t max_frames) {
  ResetParseState();
  error_ = WavError::None;

  const std::byte *data = file_data_.data();
  const size_t size = file_data_.size();
  if (size < 12 || !IdEquals(data, "RIFF")) {
    error_ = WavError::NotRiffFile;
    return false;
  }
  if (!IdEquals(data + 8, "WAVE")) {
    error_ = WavError::NotWaveFile;
    return false;
  }

  // The chunk walk below works exclusively in integer offsets relative to
  // the buffer base, validated against the remaining byte count before any
  // pointer is formed. No pointer is ever advanced by an attacker-
  // controlled value, so oversized or wrapping chunk sizes cannot produce
  // out-of-bounds accesses (and the walk always terminates).
  const uint32_t declared_riff = ReadU32(data + 4);
  size_t limit = size;
  if (declared_riff > size - 8) {
    error_ = WavError::OversizedRiffChunk;
  }
  else if (static_cast<size_t>(8) + declared_riff < size) {
    error_ = WavError::TrailingJunk;
    limit = static_cast<size_t>(8) + declared_riff;
  }

  size_t fmt_off = 0;
  size_t fmt_size = 0;
  size_t data_off = 0;
  size_t data_avail = 0;
  bool have_fmt = false;
  bool have_data = false;
  bool data_truncated = false;

  if (limit >= 12) {
    size_t pos = 12;
    while (limit - pos >= 8) {
      const uint32_t declared = ReadU32(data + pos + 4);
      const size_t chunk_data_off = pos + 8;
      const size_t remaining = limit - chunk_data_off;
      const size_t used = declared < remaining ? static_cast<size_t>(declared) : remaining;
      if (IdEquals(data + pos, "fmt ")) {
        fmt_off = chunk_data_off;
        fmt_size = used;
        have_fmt = true;
      }
      else if (IdEquals(data + pos, "data")) {
        data_off = chunk_data_off;
        data_avail = used;
        have_data = true;
        if (static_cast<size_t>(declared) > remaining) {
          data_truncated = true;
        }
      }
      const uint64_t next = static_cast<uint64_t>(chunk_data_off) + declared + (declared & 1u);
      if (next > limit) {
        break;
      }
      pos = static_cast<size_t>(next);
    }
  }

  if (!have_fmt) {
    error_ = WavError::MissingFormatChunk;
    return false;
  }
  if (fmt_size < 16) {
    error_ = WavError::InvalidFormatChunk;
    return false;
  }

  uint16_t format_tag = ReadU16(data + fmt_off);
  channel_count_ = ReadU16(data + fmt_off + 2);
  sample_rate_ = ReadU32(data + fmt_off + 4);
  uint16_t bits = ReadU16(data + fmt_off + 14);
  unsigned valid_bits = 0;
  if (format_tag == 0xFFFE) {
    if (fmt_size < 40) {
      error_ = WavError::InvalidFormatChunk;
      return false;
    }
    valid_bits = ReadU16(data + fmt_off + 18);
    format_tag = ReadU16(data + fmt_off + 24);
  }

  if (channel_count_ == 0 || sample_rate_ == 0) {
    error_ = WavError::InvalidFormatChunk;
    return false;
  }

  if (format_tag == 1) {
    switch (bits) {
    case 8:
      data_type_ = WavDataType::Unsigned8;
      is_unsigned_ = true;
      break;
    case 12:
      data_type_ = WavDataType::Signed16;
      break;
    case 16:
      data_type_ = WavDataType::Signed16;
      break;
    case 24:
      data_type_ = WavDataType::Signed24;
      break;
    case 32:
      data_type_ = WavDataType::Signed32;
      break;
    default:
      error_ = WavError::InvalidFormatChunk;
      return false;
    }
  }
  else if (format_tag == 3) {
    switch (bits) {
    case 32:
      data_type_ = WavDataType::Float32;
      is_float_ = true;
      break;
    case 64:
      data_type_ = WavDataType::Float64;
      is_float_ = true;
      break;
    default:
      error_ = WavError::InvalidFormatChunk;
      return false;
    }
  }
  else {
    error_ = WavError::UnsupportedFormat;
    return false;
  }

  container_bytes_ = static_cast<uint16_t>((bits + 7) / 8);
  const unsigned container_bits = static_cast<unsigned>(container_bytes_) * 8;
  const unsigned effective_valid = valid_bits != 0 && valid_bits < container_bits ? valid_bits : (bits < container_bits ? bits : container_bits);
  shift_ = container_bits - effective_valid;
  valid_bits_ = is_float_ ? container_bits : effective_valid;
  full_scale_ = is_unsigned_ ? 128.0 : static_cast<double>(1u << (effective_valid - 1));

  if (!have_data) {
    error_ = WavError::MissingDataChunk;
    return false;
  }

  const size_t block = static_cast<size_t>(container_bytes_) * channel_count_;
  frame_count_ = data_avail / block;
  if (frame_count_ == 0) {
    error_ = WavError::EmptyDataChunk;
    return false;
  }
  if (max_frames != 0 && frame_count_ > max_frames) {
    ResetParseState();
    error_ = WavError::TooManyFrames;
    return false;
  }
  // From here on only partial errors can occur: the data read so far
  // stays available through the query and getter functions.
  data_begin_ = data + data_off;
  data_end_ = data_begin_ + data_avail;
  if (data_truncated) {
    error_ = WavError::DataTruncated;
    return false;
  }
  return true;
}

bool WavFile::LoadFile(const std::string &path, uint64_t max_frames) {
  ResetParseState();
  error_ = WavError::None;
  file_data_.clear();
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    error_ = WavError::FileNotFound;
    return false;
  }
  LoadStream(stream, max_frames);
  return !HasError();
}

bool WavFile::LoadStream(std::istream &stream, uint64_t max_frames) {
  ResetParseState();
  error_ = WavError::None;
  file_data_.clear();
  char buffer[4096];
  while (stream.read(buffer, sizeof(buffer)) || stream.gcount() > 0) {
    const size_t count = static_cast<size_t>(stream.gcount());
    file_data_.insert(file_data_.end(), reinterpret_cast<std::byte *>(buffer),
                      reinterpret_cast<std::byte *>(buffer) + count);
    if (!stream) {
      break;
    }
  }
  if (stream.bad()) {
    ResetParseState();
    error_ = WavError::ReadError;
    return false;
  }
  ParseBuffer(max_frames);
  return !HasError();
}

bool WavFile::LoadData(const std::vector<std::byte> &data, uint64_t max_frames) {
  file_data_ = data;
  ParseBuffer(max_frames);
  return !HasError();
}

#if defined(WAV_LOADER_HAVE_SPAN)
bool WavFile::LoadData(std::span<const std::byte> data, uint64_t max_frames) {
  file_data_.assign(data.begin(), data.end());
  ParseBuffer(max_frames);
  return !HasError();
}
#endif

void WavFile::Clear() {
  // Zero the buffer before releasing it so confidential audio content
  // does not linger in freed heap memory.
  if (!file_data_.empty()) {
    memset(file_data_.data(), 0, file_data_.size());
  }
  file_data_.clear();
  file_data_.shrink_to_fit();
  ResetParseState();
  error_ = WavError::None;
}

const char *WavFile::ErrorString() const {
  switch (error_) {
  case WavError::None:
    return "no error";
  case WavError::FileNotFound:
    return "file not found";
  case WavError::ReadError:
    return "stream read error";
  case WavError::NotRiffFile:
    return "not a RIFF file";
  case WavError::NotWaveFile:
    return "not a WAVE file";
  case WavError::MissingFormatChunk:
    return "missing format chunk";
  case WavError::InvalidFormatChunk:
    return "invalid format chunk";
  case WavError::UnsupportedFormat:
    return "unsupported sample format (only PCM is supported)";
  case WavError::MissingDataChunk:
    return "missing data chunk";
  case WavError::EmptyDataChunk:
    return "data chunk contains no samples";
  case WavError::TooManyFrames:
    return "file has more frames than the maximum allowed for loading";
  case WavError::DataTruncated:
    return "missing sample data in the file (truncated data chunk)";
  case WavError::TrailingJunk:
    return "trailing data after the RIFF chunk";
  case WavError::OversizedRiffChunk:
    return "declared RIFF chunk size is larger than the available data";
  default:
    return "unknown error";
  }
}

template <typename T>
std::vector<std::vector<T>> WavFile::GetAs() const {
  std::vector<std::vector<T>> channels(channel_count_);
  if (channel_count_ == 0) {
    return channels;
  }
  const size_t block = static_cast<size_t>(container_bytes_) * channel_count_;
  const bool source_float = is_float_;
  // Dither only when precision is reduced: float to int, or int to a
  // lower bit depth. Up-conversions (and float to float) are exact.
  const bool dither = source_float ? !DestTraits<T>::is_float : (DestTraits<T>::bits < valid_bits_);
  double norm = 1.0;
  if (source_float && !DestTraits<T>::is_float) {
    norm = ScanMaxAbs();
    if (norm <= 0.0) {
      norm = 1.0;
    }
  }
  for (uint16_t c = 0; c < channel_count_; ++c) {
    std::vector<T> &channel = channels[c];
    channel.resize(static_cast<size_t>(frame_count_));
    for (uint64_t i = 0; i < frame_count_; ++i) {
      const std::byte *pos = data_begin_ + i * block + c * container_bytes_;
      double value = ReadSampleDouble(pos);
      if (!DestTraits<T>::is_float) {
        // Non-finite samples (NaN, Inf) would make the conversion to an
        // integer type undefined behavior; convert them to zero instead.
        if (!isfinite(value)) {
          value = 0.0;
        }
        value /= norm;
        if (value > 1.0) {
          value = 1.0;
        }
        else if (value < -1.0) {
          value = -1.0;
        }
      }
      channel[static_cast<size_t>(i)] = DoubleToDest<T>(value, dither ? DitherNoise() : 0.0, source_float);
    }
  }
  return channels;
}

template std::vector<std::vector<uint8_t>> WavFile::GetAs<uint8_t>() const;
template std::vector<std::vector<int16_t>> WavFile::GetAs<int16_t>() const;
template std::vector<std::vector<int32_t>> WavFile::GetAs<int32_t>() const;
template std::vector<std::vector<float>> WavFile::GetAs<float>() const;
template std::vector<std::vector<double>> WavFile::GetAs<double>() const;

std::vector<std::vector<uint8_t>> WavFile::GetUnsigned8() {
  if (IsFatal()) {
    return {};
  }
  return GetAs<uint8_t>();
}

std::vector<std::vector<int16_t>> WavFile::GetSigned16() {
  if (IsFatal()) {
    return {};
  }
  return GetAs<int16_t>();
}

std::vector<std::vector<int32_t>> WavFile::GetSigned24() {
  if (IsFatal()) {
    return {};
  }
  return GetAs<int32_t>();
}

std::vector<std::vector<int32_t>> WavFile::GetSigned32() {
  if (IsFatal()) {
    return {};
  }
  return GetAs<int32_t>();
}

std::vector<std::vector<float>> WavFile::GetFloat32() {
  if (IsFatal()) {
    return {};
  }
  return GetAs<float>();
}

std::vector<std::vector<double>> WavFile::GetFloat64() {
  if (IsFatal()) {
    return {};
  }
  return GetAs<double>();
}

} // namespace wav
