// Проверка врезки захвата на настоящем APM движка без устройств: синтетический
// голос со щелчками клавиатуры прогоняется через RTCAudioProcessingSandbox с
// SqueakCaptureProcessor и без него, метрики сравниваются. Запускается в CI
// плагина на каждый пуш и в CI клиента на собранном бандле.
//
//   squeak_apm_test [--cases full|smoke] [--expect-df ready|missing|any]
//                   [--bundle <dir>]
//
// Коды выхода: 0 — всё прошло, 1 — метрики, 2 — окружение (DFN3, бандл),
// 64 — аргументы.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__)
#include <dlfcn.h>
#include <link.h>
#endif

#include "rtc_audio_processing_sandbox.h"
#include "squeak_capture_processor.h"
#include "squeak_resampler.h"

namespace {

using flutter_webrtc_plugin::SqueakCaptureProcessor;
using flutter_webrtc_plugin::SqueakDeepFilter;
using libwebrtc::RTCAudioProcessingSandbox;

constexpr double kPi = 3.14159265358979323846;
constexpr int kExitMetrics = 1;
constexpr int kExitEnvironment = 2;
constexpr int kExitUsage = 64;

// Раскладка сигнала, секунды: щелчки → голос со щелчками → щелчки.
constexpr double kTotalSec = 6.0;
constexpr double kSpeechFromSec = 1.5;
constexpr double kSpeechToSec = 4.0;
// Хвост без голоса: 300 мс запаса на удержание гейта и задержку сети.
constexpr double kTailFromSec = 4.3;

constexpr double kSpeechDbfs = -26.0;
constexpr double kClickPeakDbfs = -20.0;
// Собственный шум микрофона, RMS.
constexpr double kFloorDbfs = -60.0;

// Допуск по голосу. Синтетический голос для сети «страннее» живой речи: на
// живой записи DFN3 держит голос в пределах 0,5 дБ, на синтетике через
// настоящий APM снимает до 4,2 дБ, а с AGC1 — до 7: тот поднимает уровень
// вместе с шумовым полом, и SNR синтетики падает. Пороги ловят разрушение
// голоса (−25 дБ и хуже было при стерильном сигнале без шумового пола), а не
// нюансы качества — те меряются на живой речи (--speech) и на стенде.
constexpr double kVoiceToleranceDb = 5.0;
constexpr double kVoiceToleranceAgcDb = 8.0;

// Ослабление щелчков в паузах: DFN3 на живой записи даёт 35–40 дБ, RNNoise с
// гейтом — 15–20; пороги вдвое ниже, чтобы ловить выпадение из тракта.
constexpr double kClickDeepDb = 20.0;
constexpr double kClickRnnoiseDb = 8.0;

struct Options {
  std::string cases = "full";
  std::string expect_df = "any";
  std::string bundle;
  // WAV 16 бит (моно или стерео) вместо синтетического голоса: живая речь
  // через настоящий APM, с AGC и без — цифры качества, а не только тракта.
  std::string speech;
};

// Голос из файла: сводится в моно, пересчитывается на частоту кейса тем же
// ресемплером, что в процессоре, обрезается до нужной длины.
std::vector<float> g_speech_file;
int g_speech_file_rate = 0;

bool ReadWav16(const std::string& path, std::vector<float>* mono, int* rate) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return false;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size < 44) {
    std::fclose(f);
    return false;
  }
  std::vector<unsigned char> raw(static_cast<size_t>(size));
  const size_t got = std::fread(raw.data(), 1, raw.size(), f);
  std::fclose(f);
  if (got != raw.size() || std::memcmp(raw.data(), "RIFF", 4) != 0) return false;
  int channels = 0;
  size_t pos = 12;
  while (pos + 8 <= raw.size()) {
    uint32_t len;
    std::memcpy(&len, &raw[pos + 4], 4);
    if (std::memcmp(&raw[pos], "fmt ", 4) == 0 && len >= 16) {
      uint16_t format, ch, bits;
      uint32_t sr;
      std::memcpy(&format, &raw[pos + 8], 2);
      std::memcpy(&ch, &raw[pos + 10], 2);
      std::memcpy(&sr, &raw[pos + 12], 4);
      std::memcpy(&bits, &raw[pos + 22], 2);
      // 1 — PCM, 0xFFFE — WAVE_FORMAT_EXTENSIBLE (так пишет afconvert).
      if ((format != 1 && format != 0xFFFE) || bits != 16 || ch == 0) {
        return false;
      }
      channels = ch;
      *rate = static_cast<int>(sr);
    } else if (std::memcmp(&raw[pos], "data", 4) == 0 && channels > 0) {
      const size_t frames = std::min<size_t>(len, raw.size() - pos - 8) / 2 / channels;
      mono->resize(frames);
      for (size_t i = 0; i < frames; ++i) {
        double sum = 0;
        for (int c = 0; c < channels; ++c) {
          int16_t s;
          std::memcpy(&s, &raw[pos + 8 + (i * channels + c) * 2], 2);
          sum += s;
        }
        (*mono)[i] = static_cast<float>(sum / channels / 32768.0);
      }
      return true;
    }
    pos += 8 + len + (len & 1);
  }
  return false;
}

std::vector<float> SpeechFromFile(int rate, double seconds) {
  const int n = static_cast<int>(rate * seconds);
  std::vector<float> src = g_speech_file;
  if (g_speech_file_rate != rate) {
    // Пачками по 10 мс входной частоты, как в APM.
    int a = g_speech_file_rate, b = rate;
    while (b != 0) {
      const int t = a % b;
      a = b;
      b = t;
    }
    flutter_webrtc_plugin::SqueakResampler resampler(rate / a, g_speech_file_rate / a);
    const int in_frame = g_speech_file_rate / 100;
    std::vector<float> out;
    std::vector<float> chunk(static_cast<size_t>(rate / 100) + 16);
    for (size_t off = 0; off + in_frame <= src.size(); off += in_frame) {
      const int produced = resampler.Process(&src[off], in_frame, chunk.data());
      out.insert(out.end(), chunk.begin(), chunk.begin() + produced);
    }
    src = std::move(out);
  }
  // Первые полсекунды файла часто тишина или щелчок записи — пропускаем.
  const size_t skip = std::min<size_t>(src.size(), static_cast<size_t>(rate / 2));
  std::vector<float> out(n, 0.0f);
  for (int i = 0; i < n && skip + i < src.size(); ++i) out[i] = src[skip + i];
  double energy = 0;
  for (float x : out) energy += static_cast<double>(x) * x;
  if (energy > 0) {
    const double gain = std::pow(10.0, kSpeechDbfs / 20.0) / std::sqrt(energy / n);
    for (float& x : out) x = static_cast<float>(x * gain);
  }
  return out;
}

struct Case {
  int rate;
  int channels;
  bool agc;
  bool ns;
  bool aec;
};

struct Rng {
  uint64_t s = 0x9E3779B97F4A7C15ull;
  double uniform() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return static_cast<double>(s >> 11) * (1.0 / 9007199254740992.0);
  }
  double bipolar() { return 2.0 * uniform() - 1.0; }
};

double Db(double ratio) { return 20.0 * std::log10(std::max(ratio, 1e-12)); }

// Голос: гармоники f0 120–200 Гц под тремя формантами, гласные /a/ /i/ /u/
// сменяются плавно, слоговая огибающая 4 Гц, шумовой всплеск на онсете.
std::vector<float> MakeSpeech(int rate, double seconds) {
  const int n = static_cast<int>(rate * seconds);
  std::vector<float> out(n, 0.0f);
  const double formants[3][3] = {
      {730, 1090, 2440}, {270, 2290, 3010}, {300, 870, 2240}};
  const double max_harmonic = std::min(4000.0, 0.45 * rate);
  double phase[64] = {};
  Rng rng;
  for (int i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / rate;
    const double f0 = 160 + 40 * std::sin(2 * kPi * 0.5 * t) +
                      5 * std::sin(2 * kPi * 5.5 * t);
    const double syllable = std::max(0.0, std::sin(2 * kPi * 4 * t));
    const double env = 0.25 + 0.75 * std::sqrt(syllable);
    const double v = std::fmod(t / 0.4, 3.0);
    const int va = static_cast<int>(v);
    const int vb = (va + 1) % 3;
    const double mix = v - va;
    double sample = 0;
    for (int k = 1; k < 64; ++k) {
      const double f = k * f0;
      if (f > max_harmonic) break;
      double amp = 0;
      for (int m = 0; m < 3; ++m) {
        const double fc = formants[va][m] * (1 - mix) + formants[vb][m] * mix;
        const double d = (f - fc) / (90 + 0.04 * fc);
        amp += std::exp(-d * d);
      }
      amp = (amp + 0.05) / k;
      phase[k] += 2 * kPi * f / rate;
      if (phase[k] > 2 * kPi) phase[k] -= 2 * kPi;
      sample += amp * std::sin(phase[k]);
    }
    const double onset = std::fmod(t, 0.25);
    if (onset < 0.02) sample += 0.4 * rng.bipolar() * (1 - onset / 0.02);
    out[i] = static_cast<float>(sample * env);
  }
  double energy = 0;
  for (float x : out) energy += static_cast<double>(x) * x;
  const double gain = std::pow(10.0, kSpeechDbfs / 20.0) / std::sqrt(energy / n);
  for (float& x : out) x = static_cast<float>(x * gain);
  return out;
}

// Щелчки клавиатуры: шумовые всплески 10–15 мс с экспоненциальным спадом,
// каждые ~250 мс.
std::vector<float> MakeClicks(int rate, double seconds) {
  const int n = static_cast<int>(rate * seconds);
  std::vector<float> out(n, 0.0f);
  const double peak = std::pow(10.0, kClickPeakDbfs / 20.0);
  const double tau = rate * 0.003;
  Rng rng;
  rng.s ^= 0x5bd1e995u;
  double t = 0.1;
  while (t < seconds) {
    const int start = static_cast<int>(t * rate);
    const int len = static_cast<int>(rate * (0.010 + 0.005 * rng.uniform()));
    for (int k = 0; k < len && start + k < n; ++k) {
      out[start + k] +=
          static_cast<float>(peak * rng.bipolar() * std::exp(-k / tau));
    }
    t += 0.25 + 0.06 * rng.bipolar();
  }
  return out;
}

struct Signal {
  int rate = 0;
  int channels = 0;
  // Чистый голос (моно), щелчки и собственный шум микрофона по каналам,
  // в [-1, 1]. Шумовой пол есть у любого настоящего микрофона; без него
  // оценка SNR в сети уходит в бесконечность и ведёт себя не как в жизни.
  std::vector<float> speech;
  std::vector<std::vector<float>> clicks;
  std::vector<std::vector<float>> floor;

  std::vector<int16_t> Interleaved(bool with_speech, bool with_clicks) const {
    const size_t n = speech.size();
    std::vector<int16_t> out(n * channels);
    for (size_t i = 0; i < n; ++i) {
      for (int c = 0; c < channels; ++c) {
        double v = floor[c][i];
        if (with_speech) v += speech[i];
        if (with_clicks) v += clicks[c][i];
        out[i * channels + c] = static_cast<int16_t>(
            std::lround(std::clamp(v, -1.0, 1.0) * 32767.0));
      }
    }
    return out;
  }
};

Signal MakeSignal(int rate, int channels) {
  Signal s;
  s.rate = rate;
  s.channels = channels;
  const int n = static_cast<int>(rate * kTotalSec);
  s.speech.assign(n, 0.0f);
  const std::vector<float> voice =
      g_speech_file.empty() ? MakeSpeech(rate, kSpeechToSec - kSpeechFromSec)
                            : SpeechFromFile(rate, kSpeechToSec - kSpeechFromSec);
  const int from = static_cast<int>(rate * kSpeechFromSec);
  const int fade = rate / 50;
  for (size_t i = 0; i < voice.size() && from + i < s.speech.size(); ++i) {
    double w = 1.0;
    if (i < static_cast<size_t>(fade)) w = static_cast<double>(i) / fade;
    const size_t left = voice.size() - 1 - i;
    if (left < static_cast<size_t>(fade)) w = std::min(w, left / double(fade));
    s.speech[from + i] = static_cast<float>(voice[i] * w);
  }
  const std::vector<float> clicks = MakeClicks(rate, kTotalSec);
  s.clicks.push_back(clicks);
  for (int c = 1; c < channels; ++c) {
    // Второй канал — те же щелчки на 3 мс позже: стерео, а не копия.
    std::vector<float> shifted(n, 0.0f);
    const int shift = rate * 3 / 1000;
    for (int i = shift; i < n; ++i) shifted[i] = clicks[i - shift];
    s.clicks.push_back(shifted);
  }
  Rng rng;
  rng.s ^= 0x2545F4914F6CDD1Dull;
  const float floor_amp = static_cast<float>(std::pow(10.0, kFloorDbfs / 20.0));
  for (int c = 0; c < channels; ++c) {
    std::vector<float> noise(n);
    for (int i = 0; i < n; ++i) {
      noise[i] = static_cast<float>(rng.bipolar() * floor_amp * 1.7320508);
    }
    s.floor.push_back(noise);
  }
  return s;
}

// Фиктивный процессор: считает вызовы, проверяет контракт отсоединения.
class CountingProcessor
    : public libwebrtc::RTCAudioProcessing::CustomProcessing {
 public:
  int inits = 0;
  int frames = 0;
  int releases = 0;
  void Initialize(int, int) override { ++inits; }
  void Process(int, int, int, float*) override { ++frames; }
  void Reset(int) override {}
  void Release() override { ++releases; }
};

struct RunResult {
  std::vector<float> out;
  SqueakCaptureProcessor::Stats stats;
  SqueakDeepFilter::Status df = SqueakDeepFilter::Status::kIdle;
  std::string df_path;
  int apm_error = 0;
  std::string config;
};

RTCAudioProcessingSandbox::Config SandboxConfig(const Case& c) {
  RTCAudioProcessingSandbox::Config cfg;
  cfg.gain_controller1 = c.agc;
  cfg.noise_suppression = c.ns;
  cfg.echo_canceller = c.aec;
  cfg.high_pass_filter = true;
  cfg.maximum_internal_processing_rate = 48000;
  cfg.multi_channel_capture = c.channels > 1;
  return cfg;
}

RunResult Run(const Case& c, const Signal& signal,
              const std::vector<int16_t>& in, bool with_processor) {
  RunResult result;
  auto apm = RTCAudioProcessingSandbox::Create(SandboxConfig(c));
  std::unique_ptr<SqueakCaptureProcessor> processor;
  if (with_processor) {
    processor = std::make_unique<SqueakCaptureProcessor>();
    apm->SetCapturePostProcessing(processor.get());
    processor->SetRnnoiseEnabled(true);
    const auto started = std::chrono::steady_clock::now();
    while (processor->TakeStats().deep_filter ==
               SqueakDeepFilter::Status::kLoading &&
           std::chrono::steady_clock::now() - started <
               std::chrono::seconds(5)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Снятие обнуляет счётчики: дальше считаем только кадры прогона.
    result.df = processor->TakeStats().deep_filter;
    result.df_path = processor->deep_filter().library_path();
  }

  const int frame = c.rate / 100;
  const int stride = frame * c.channels;
  std::vector<int16_t> out(in.size(), 0);
  // Эхо-референс для AEC3: голос с задержкой 30 мс, будто играет из колонок.
  const std::vector<int16_t> render = signal.Interleaved(true, false);
  const size_t render_delay = static_cast<size_t>(stride) * 3;
  for (size_t off = 0; off + stride <= in.size(); off += stride) {
    if (c.aec) {
      apm->SetStreamDelayMs(30);
      if (off >= render_delay) {
        apm->ProcessRender(&render[off - render_delay], c.rate, c.channels);
      }
    }
    const int err = apm->ProcessCapture(&in[off], c.rate, c.channels, &out[off]);
    if (err != 0) result.apm_error = err;
  }
  result.config = apm->GetConfigSummary().std_string();
  if (processor) {
    result.stats = processor->TakeStats();
    apm->SetCapturePostProcessing(nullptr);
  }
  result.out.resize(out.size());
  for (size_t i = 0; i < out.size(); ++i) result.out[i] = out[i] / 32768.0f;
  return result;
}

double Rms(const std::vector<float>& x, int channels, int ch, int from,
           int to) {
  double e = 0;
  int n = 0;
  for (int i = from; i < to; ++i) {
    const size_t idx = static_cast<size_t>(i) * channels + ch;
    if (idx >= x.size()) break;
    e += static_cast<double>(x[idx]) * x[idx];
    ++n;
  }
  return n == 0 ? 0 : std::sqrt(e / n);
}

double Dot(const std::vector<float>& a, const std::vector<float>& b,
           int channels, int ch, int from, int to, int lag) {
  double s = 0;
  for (int i = from; i < to; ++i) {
    const size_t ia = static_cast<size_t>(i) * channels + ch;
    const size_t ib = static_cast<size_t>(i + lag) * channels + ch;
    if (ia >= a.size() || ib >= b.size()) break;
    s += static_cast<double>(a[ia]) * b[ib];
  }
  return s;
}

// Задержка выхода относительно опоры: максимум корреляции на голосе.
int FindDelay(const std::vector<float>& ref, const std::vector<float>& y,
              int channels, int rate) {
  const int from = static_cast<int>(rate * (kSpeechFromSec + 0.3));
  const int to = static_cast<int>(rate * (kSpeechToSec - 0.3));
  const int max_lag = rate * 60 / 1000;
  const int coarse = std::max(1, rate / 6000);
  int best = 0;
  double best_c = -1e300;
  for (int lag = 0; lag <= max_lag; lag += coarse) {
    const double c = Dot(ref, y, channels, 0, from, to, lag);
    if (c > best_c) {
      best_c = c;
      best = lag;
    }
  }
  int fine = best;
  for (int lag = std::max(0, best - coarse); lag <= best + coarse; ++lag) {
    const double c = Dot(ref, y, channels, 0, from, to, lag);
    if (c > best_c) {
      best_c = c;
      fine = lag;
    }
  }
  return fine;
}

struct Metrics {
  double den = 0;
  double click_db = 0;
  double voice_db = 0;
  double delay_ms = 0;
  double stereo_diff = 0;
};

Metrics Measure(const Case& c, const RunResult& ref,
                const RunResult& voice_ref, const RunResult& test) {
  Metrics m;
  const int rate = c.rate;
  const int delay = FindDelay(ref.out, test.out, c.channels, rate);
  m.delay_ms = delay * 1000.0 / rate;
  const int tail_from = static_cast<int>(rate * kTailFromSec);
  const int tail_to = static_cast<int>(rate * kTotalSec) - delay - 1;
  const int sp_from = static_cast<int>(rate * (kSpeechFromSec + 0.3));
  const int sp_to = static_cast<int>(rate * (kSpeechToSec - 0.3));
  m.click_db = 1e9;
  m.voice_db = 0;
  for (int ch = 0; ch < c.channels; ++ch) {
    const double before = Rms(ref.out, c.channels, ch, tail_from, tail_to);
    const double after =
        Rms(test.out, c.channels, ch, tail_from + delay, tail_to + delay);
    m.click_db = std::min(m.click_db, Db(before / std::max(after, 1e-9)));
    // Проекция выхода на опорный голос: как сохранён голос.
    const double num = Dot(voice_ref.out, test.out, c.channels, ch, sp_from,
                           sp_to, delay);
    const double den = Dot(voice_ref.out, voice_ref.out, c.channels, ch,
                           sp_from, sp_to, 0);
    const double gain_db = Db(num / std::max(den, 1e-12));
    if (ch == 0 || std::fabs(gain_db) > std::fabs(m.voice_db)) {
      m.voice_db = gain_db;
    }
  }
  if (c.channels > 1) {
    for (size_t i = 0; i + 1 < test.out.size(); i += c.channels) {
      m.stereo_diff =
          std::max(m.stereo_diff, double(std::fabs(test.out[i] - test.out[i + 1])));
    }
  }
  const auto& st = test.stats;
  m.den = st.frames == 0 ? 0 : double(st.denoised) / double(st.frames);
  return m;
}

std::string Normalize(std::string path) {
  std::replace(path.begin(), path.end(), '\\', '/');
#if defined(_WIN32)
  std::transform(path.begin(), path.end(), path.begin(),
                 [](unsigned char ch) { return char(std::tolower(ch)); });
#endif
  while (!path.empty() && path.back() == '/') path.pop_back();
  return path;
}

std::string EngineLibraryPath() {
#if defined(_WIN32)
  HMODULE module = GetModuleHandleW(L"libwebrtc.dll");
  if (module == nullptr) return "";
  wchar_t buf[MAX_PATH];
  const DWORD n = GetModuleFileNameW(module, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return "";
  std::string out;
  for (DWORD i = 0; i < n; ++i) out.push_back(static_cast<char>(buf[i]));
  return out;
#elif !defined(__linux__)
  return "";
#else
  void* handle = dlopen("libwebrtc.so", RTLD_NOW | RTLD_NOLOAD);
  if (handle == nullptr) return "";
  struct link_map* map = nullptr;
  std::string out;
  if (dlinfo(handle, RTLD_DI_LINKMAP, &map) == 0 && map != nullptr &&
      map->l_name != nullptr) {
    out = map->l_name;
  }
  dlclose(handle);
  return out;
#endif
}

bool Under(const std::string& path, const std::string& dir) {
  if (path.empty() || dir.empty()) return false;
  const std::string p = Normalize(path);
  const std::string d = Normalize(dir) + "/";
  return p.compare(0, d.size(), d) == 0;
}

bool ParseArgs(int argc, char** argv, Options* options) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = i + 1 < argc;
    if (arg == "--cases" && has_value) {
      options->cases = argv[++i];
    } else if (arg == "--expect-df" && has_value) {
      options->expect_df = argv[++i];
    } else if (arg == "--bundle" && has_value) {
      options->bundle = argv[++i];
    } else if (arg == "--speech" && has_value) {
      options->speech = argv[++i];
    } else {
      std::fprintf(stderr, "неизвестный аргумент: %s\n", arg.c_str());
      return false;
    }
  }
  if (options->cases != "full" && options->cases != "smoke") return false;
  if (options->expect_df != "ready" && options->expect_df != "missing" &&
      options->expect_df != "any") {
    return false;
  }
  return true;
}

const char* DfName(SqueakDeepFilter::Status s) {
  switch (s) {
    case SqueakDeepFilter::Status::kIdle: return "idle";
    case SqueakDeepFilter::Status::kLoading: return "loading";
    case SqueakDeepFilter::Status::kReady: return "ready";
    case SqueakDeepFilter::Status::kMissing: return "missing";
    case SqueakDeepFilter::Status::kFailed: return "failed";
  }
  return "?";
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!ParseArgs(argc, argv, &options)) {
    std::fprintf(stderr,
                 "squeak_apm_test [--cases full|smoke] "
                 "[--expect-df ready|missing|any] [--bundle <dir>] "
                 "[--speech <wav>]\n");
    return kExitUsage;
  }
  if (!options.speech.empty()) {
    if (!ReadWav16(options.speech, &g_speech_file, &g_speech_file_rate) ||
        g_speech_file.size() < static_cast<size_t>(g_speech_file_rate)) {
      std::fprintf(stderr, "--speech: нужен WAV PCM 16 бит длиннее секунды: %s\n",
                   options.speech.c_str());
      return kExitUsage;
    }
    std::printf("speech from %s (%d Hz, %.1f s)\n", options.speech.c_str(),
                g_speech_file_rate,
                static_cast<double>(g_speech_file.size()) / g_speech_file_rate);
  }

  int passed = 0;
  int failed = 0;
  bool environment_ok = true;
  std::string df_path;
  std::string apm_config;

  // Контракт отсоединения: Release ровно один раз, после — без вызовов.
  {
    auto apm = RTCAudioProcessingSandbox::Create(SandboxConfig({48000, 1, false, false, false}));
    CountingProcessor counting;
    apm->SetCapturePostProcessing(&counting);
    std::vector<int16_t> frame(480, 0);
    for (int i = 0; i < 10; ++i) apm->ProcessCapture(frame.data(), 48000, 1, frame.data());
    apm->SetCapturePostProcessing(nullptr);
    const int frames_at_detach = counting.frames;
    for (int i = 0; i < 10; ++i) apm->ProcessCapture(frame.data(), 48000, 1, frame.data());
    apm->SetCapturePostProcessing(&counting);
    apm->SetCapturePostProcessing(nullptr);
    const bool ok = counting.releases == 2 && frames_at_detach == 10 &&
                    counting.frames == frames_at_detach && counting.inits >= 1;
    std::printf("case detach | releases=%d frames=%d inits=%d | %s\n",
                counting.releases, counting.frames, counting.inits,
                ok ? "PASS" : "FAIL");
    if (ok) ++passed; else {
      ++failed;
      std::printf("::error::detach: Release не вызван ровно по разу или Process звался после отсоединения\n");
    }
  }

  std::vector<Case> cases;
  if (options.cases == "smoke") {
    cases.push_back({48000, 1, true, true, true});
  } else {
    for (int rate : {8000, 16000, 32000, 48000}) {
      for (int channels : {1, 2}) {
        for (bool agc : {false, true}) {
          cases.push_back({rate, channels, agc, false, false});
        }
      }
    }
  }

  for (const Case& c : cases) {
    const Signal signal = MakeSignal(c.rate, c.channels);
    const std::vector<int16_t> noisy = signal.Interleaved(true, true);
    const RunResult ref = Run(c, signal, noisy, false);
    // Опора для голоса: без AGC цепочка линейна, и чистый голос через тот же
    // APM — честная опора; с AGC усиление зависит от уровня, опора — вход
    // со щелчками.
    const RunResult voice_ref =
        c.agc ? ref : Run(c, signal, signal.Interleaved(true, false), false);
    const RunResult test = Run(c, signal, noisy, true);
    apm_config = test.config;
    if (!test.df_path.empty()) df_path = test.df_path;

    const Metrics m = Measure(c, ref, voice_ref, test);
    const bool deep = test.df == SqueakDeepFilter::Status::kReady;
    const bool smoke = options.cases == "smoke";
    std::string why;
    if (test.apm_error != 0 || ref.apm_error != 0) why += " apm_error";
    if (options.expect_df == "ready" && !deep) { why += " df_not_ready"; environment_ok = false; }
    if (options.expect_df == "missing" && test.df != SqueakDeepFilter::Status::kMissing) { why += " df_present"; environment_ok = false; }
    if (m.den < 0.99) why += " den";
    // 8 кГц — телефонная полоса (Bluetooth HFP): обе сети там слабы, метрики
    // печатаем, но не судим; тракт и каналы проверяются как везде.
    const bool narrowband = c.rate == 8000;
    if (!smoke) {
      if (!narrowband && m.click_db < (deep ? kClickDeepDb : kClickRnnoiseDb)) {
        why += " click";
      }
      const double voice_limit = c.agc ? kVoiceToleranceAgcDb : kVoiceToleranceDb;
      if (!narrowband && std::fabs(m.voice_db) > voice_limit) why += " voice";
      if (c.channels > 1 && m.stereo_diff > 1e-3) why += " stereo";
    } else {
      bool finite = true;
      for (float v : test.out) finite = finite && std::isfinite(v);
      if (!finite) why += " nan";
      const int sp_from = static_cast<int>(c.rate * (kSpeechFromSec + 0.3));
      const int sp_to = static_cast<int>(c.rate * (kSpeechToSec - 0.3));
      if (Db(Rms(test.out, c.channels, 0, sp_from, sp_to)) < -60) why += " silent";
    }
    const bool ok = why.empty();
    std::printf(
        "case rate=%d ch=%d agc=%s ns=%s aec=%s df=%s | den=%.1f%% "
        "click=%+.1fdB voice=%+.1fdB lsnr=%.1f delay=%.1fms stereo=%.4f "
        "p99=%dus max=%dus hz=%d | %s%s\n",
        c.rate, c.channels, c.agc ? "on" : "off", c.ns ? "on" : "off",
        c.aec ? "on" : "off", DfName(test.df), m.den * 100, m.click_db,
        m.voice_db, test.stats.lsnr_mean, m.delay_ms, m.stereo_diff,
        test.stats.frame_us_p99, test.stats.frame_us_max,
        test.stats.sample_rate_hz, ok ? "PASS" : "FAIL", why.c_str());
    if (test.stats.frame_us_p99 > 8000) {
      std::printf("::warning::rate=%d ch=%d: p99 времени кадра %d мкс\n", c.rate,
                  c.channels, test.stats.frame_us_p99);
    }
    if (ok) {
      ++passed;
    } else {
      ++failed;
      std::printf("::error::case rate=%d ch=%d agc=%s df=%s провален:%s\n", c.rate,
                  c.channels, c.agc ? "on" : "off", DfName(test.df), why.c_str());
    }
  }

  const std::string engine = EngineLibraryPath();
  if (!options.bundle.empty()) {
    if (!Under(engine, options.bundle)) {
      std::printf("::error::libwebrtc загружен не из бандла: %s\n", engine.c_str());
      environment_ok = false;
    }
    if (options.expect_df == "ready" && !Under(df_path, options.bundle)) {
      std::printf("::error::squeak_df загружен не из бандла: %s\n", df_path.c_str());
      environment_ok = false;
    }
  }

  std::printf("summary passed=%d failed=%d libwebrtc=%s squeak_df=%s apm=%s\n",
              passed, failed, engine.c_str(), df_path.c_str(),
              apm_config.c_str());
  if (!environment_ok) return kExitEnvironment;
  return failed == 0 ? 0 : kExitMetrics;
}
