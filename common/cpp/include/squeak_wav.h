#ifndef FLUTTER_WEBRTC_SQUEAK_WAV_HXX
#define FLUTTER_WEBRTC_SQUEAK_WAV_HXX

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace flutter_webrtc_plugin {

// WAV 16 бит моно из float в шкале int16. Для записей монитора врезки.
inline bool SqueakWriteWav(const std::string& path,
                           const std::vector<float>& samples,
                           int sample_rate_hz) {
  std::ofstream out(std::filesystem::u8path(path), std::ios::binary);
  if (!out) return false;
  const uint32_t data_bytes = static_cast<uint32_t>(samples.size() * 2);
  const uint32_t riff_bytes = 36 + data_bytes;
  const uint32_t fmt_bytes = 16;
  const uint16_t pcm = 1, channels = 1, block_align = 2, bits = 16;
  const uint32_t rate = static_cast<uint32_t>(sample_rate_hz);
  const uint32_t byte_rate = rate * block_align;
  auto put = [&out](const void* p, size_t n) {
    out.write(static_cast<const char*>(p), static_cast<std::streamsize>(n));
  };
  put("RIFF", 4);
  put(&riff_bytes, 4);
  put("WAVEfmt ", 8);
  put(&fmt_bytes, 4);
  put(&pcm, 2);
  put(&channels, 2);
  put(&rate, 4);
  put(&byte_rate, 4);
  put(&block_align, 2);
  put(&bits, 2);
  put("data", 4);
  put(&data_bytes, 4);
  for (float v : samples) {
    const int16_t s =
        static_cast<int16_t>(std::clamp(v, -32768.0f, 32767.0f));
    put(&s, 2);
  }
  return static_cast<bool>(out);
}

inline std::string SqueakTempDir() {
  std::error_code ec;
  const auto dir = std::filesystem::temp_directory_path(ec);
  return ec ? std::string() : dir.u8string();
}

}  // namespace flutter_webrtc_plugin

#endif  // FLUTTER_WEBRTC_SQUEAK_WAV_HXX
