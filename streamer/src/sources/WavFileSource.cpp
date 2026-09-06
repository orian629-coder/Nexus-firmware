#include "sources/WavFileSource.h"

#include <cstring>
#include <memory>

namespace nexus::streamer::sources {

using core::ErrorCode;
using core::Status;

namespace {

std::uint32_t le32(const unsigned char* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint16_t le16(const unsigned char* p) {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) |
                                    (static_cast<std::uint16_t>(p[1]) << 8));
}

}  // namespace

WavFileSource::~WavFileSource() {
  if (file_) std::fclose(file_);
}

core::Result<std::unique_ptr<WavFileSource>> WavFileSource::open(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return Status::error(ErrorCode::NotFound, "cannot open wav: " + path);

  unsigned char hdr[12];
  if (std::fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) || std::memcmp(hdr, "RIFF", 4) != 0 ||
      std::memcmp(hdr + 8, "WAVE", 4) != 0) {
    std::fclose(f);
    return Status::error(ErrorCode::InvalidArg, "not a RIFF/WAVE file: " + path);
  }

  auto src = std::unique_ptr<WavFileSource>(new WavFileSource());
  src->file_ = f;

  // Walk the chunk list rather than assuming fmt is immediately followed by data — real files often
  // carry LIST/INFO metadata in between.
  bool have_fmt = false;
  for (;;) {
    unsigned char ch[8];
    if (std::fread(ch, 1, sizeof(ch), f) != sizeof(ch)) break;
    const std::uint32_t size = le32(ch + 4);

    if (std::memcmp(ch, "fmt ", 4) == 0) {
      std::vector<unsigned char> fmt(size);
      if (std::fread(fmt.data(), 1, size, f) != size) break;
      const std::uint16_t format = le16(fmt.data());
      src->src_channels_ = le16(fmt.data() + 2);
      src->src_rate_ = static_cast<int>(le32(fmt.data() + 4));
      src->bits_ = le16(fmt.data() + 14);
      if (format != 1) {
        return Status::error(ErrorCode::InvalidArg,
                             "wav is not uncompressed PCM (format " + std::to_string(format) +
                                 "); use a file source that decodes it");
      }
      if (src->bits_ != 16) {
        return Status::error(ErrorCode::InvalidArg,
                             "unsupported wav bit depth: " + std::to_string(src->bits_));
      }
      if (src->src_channels_ < 1 || src->src_rate_ <= 0) {
        return Status::error(ErrorCode::InvalidArg, "wav header has invalid rate/channels");
      }
      have_fmt = true;
    } else if (std::memcmp(ch, "data", 4) == 0) {
      if (!have_fmt) return Status::error(ErrorCode::InvalidArg, "wav data chunk before fmt");
      src->data_remaining_ = size;
      return src;  // positioned at the first sample
    } else {
      // Skip unknown chunk (chunks are word-aligned).
      const long skip = static_cast<long>(size + (size & 1u));
      if (std::fseek(f, skip, SEEK_CUR) != 0) break;
    }
  }
  return Status::error(ErrorCode::InvalidArg, "wav has no data chunk: " + path);
}

// Read up to `frames` source-rate frames, widening mono to stereo so downstream only sees stereo.
std::size_t WavFileSource::readSourceFrames(std::size_t frames, std::vector<std::int16_t>& out) {
  if (frames == 0 || data_remaining_ == 0) return 0;

  const std::size_t bytes_per_frame = static_cast<std::size_t>(src_channels_) * 2u;
  const std::size_t want = std::min<std::uint64_t>(frames * bytes_per_frame, data_remaining_);
  if (want == 0) return 0;

  std::vector<unsigned char> raw(want);
  const std::size_t got = std::fread(raw.data(), 1, want, file_);
  data_remaining_ -= got;
  const std::size_t got_frames = got / bytes_per_frame;

  for (std::size_t i = 0; i < got_frames; ++i) {
    const unsigned char* p = raw.data() + i * bytes_per_frame;
    const std::int16_t l = static_cast<std::int16_t>(le16(p));
    const std::int16_t r = src_channels_ > 1 ? static_cast<std::int16_t>(le16(p + 2)) : l;
    out.push_back(l);
    out.push_back(r);
  }
  return got_frames;
}

std::size_t WavFileSource::read(std::size_t max_frames, std::vector<std::int16_t>& out) {
  if (exhausted_ || max_frames == 0) return 0;

  // Fast path: already at the wire rate, so no interpolation is needed at all.
  if (src_rate_ == 48000) {
    const std::size_t n = readSourceFrames(max_frames, out);
    if (n == 0) exhausted_ = true;
    return n;
  }

  // Resample: step through the source at rate ratio, linearly interpolating between neighbours.
  const double step = static_cast<double>(src_rate_) / 48000.0;
  std::size_t produced = 0;

  while (produced < max_frames) {
    const std::size_t need_index = static_cast<std::size_t>(pos_) + 2;  // need i and i+1
    while (!source_eof_ && pending_.size() / 2 <= need_index) {
      const std::size_t before = pending_.size();
      readSourceFrames(1024, pending_);
      if (pending_.size() == before) source_eof_ = true;
    }

    const std::size_t available = pending_.size() / 2;
    const std::size_t i = static_cast<std::size_t>(pos_);
    if (i + 1 >= available) {
      if (source_eof_) {
        exhausted_ = true;
        break;
      }
      break;
    }

    const double frac = pos_ - static_cast<double>(i);
    for (int c = 0; c < 2; ++c) {
      const double a = pending_[i * 2 + c];
      const double b = pending_[(i + 1) * 2 + c];
      out.push_back(static_cast<std::int16_t>(a + (b - a) * frac));
    }
    ++produced;
    pos_ += step;

    // Drop fully consumed history so pending_ doesn't grow without bound on a long file.
    if (pos_ > 4096.0) {
      const std::size_t drop = static_cast<std::size_t>(pos_) - 1;
      pending_.erase(pending_.begin(), pending_.begin() + static_cast<long>(drop * 2));
      pos_ -= static_cast<double>(drop);
    }
  }

  if (produced == 0) exhausted_ = true;
  return produced;
}

}  // namespace nexus::streamer::sources
