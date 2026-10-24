#include "squeak_capture_processor.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <numeric>

extern "C" {
#include "rnnoise.h"
}

namespace flutter_webrtc_plugin {
namespace {

// Столько сэмплов RNNoise берёт за раз. Совпадает с 10 мс на 48 кГц, то есть
// ровно с тем, что отдаёт APM, — переупаковка кадров не нужна.
constexpr int kRnnoiseFrameSize = 480;

constexpr int kRnnoiseRate = 48000;

// APM держит float в шкале int16. Выход за неё уводит сэмплы в клиппинг уже
// при кодировании, поэтому подрезаем сами.
constexpr float kSampleMax = 32767.0f;

// Порог вероятности голоса, с которого гейт открыт. В noise-suppression-for-voice
// (фильтр на RNNoise для OBS и PipeWire) по умолчанию 0,5, но на прогоне речи
// с щелчками клавиатуры 0,5 съедал атаку слова после паузы (40 мс), а 0,3 —
// ничего, при том же подавлении щелчков: −39,7 dBFS на входе, −49,8 после
// одного RNNoise, −65,5 с гейтом.
constexpr float kVadOpen = 0.3f;

// 200 мс: хвосты слов и короткие паузы внутри фразы гейт не рубит.
constexpr int kHoldFrames = 20;

// Открытие за 2,5 мс — первую согласную не съедаем; закрытие на весь кадр.
constexpr int kOpenRampSamples = 120;

// Верхние границы корзин времени кадра, мкс; последняя корзина — всё дольше.
constexpr int kUsBucketUpper[] = {500, 1000, 2000, 4000, 8000};

}  // namespace

struct SqueakCaptureProcessor::State {
  DenoiseState* denoiser = nullptr;

  ~State() {
    if (denoiser != nullptr) rnnoise_destroy(denoiser);
  }
};

SqueakCaptureProcessor::SqueakCaptureProcessor() = default;

SqueakCaptureProcessor::~SqueakCaptureProcessor() = default;

void SqueakCaptureProcessor::Initialize(int sample_rate_hz, int num_channels) {
  sample_rate_hz_ = sample_rate_hz;
  warned_ = false;
  if (state_ == nullptr) {
    state_ = std::make_unique<State>();
    state_->denoiser = rnnoise_create(nullptr);
  }
}

void SqueakCaptureProcessor::Reset(int new_rate) {
  sample_rate_hz_ = new_rate;
  warned_ = false;
}

void SqueakCaptureProcessor::Release() {
  state_.reset();
  sample_rate_hz_ = 0;
}

bool SqueakCaptureProcessor::EnsureResamplers(int num_frames) {
  const int rate = num_frames * 100;
  if (up_ != nullptr && resampled_rate_ == rate) return true;
  const int g = std::gcd(kRnnoiseRate, rate);
  const int up = kRnnoiseRate / g, down = rate / g;
  if (static_cast<int64_t>(num_frames) * up != int64_t{kRnnoiseFrameSize} * down) {
    return false;
  }
  up_ = std::make_unique<SqueakResampler>(up, down);
  down_ = std::make_unique<SqueakResampler>(down, up);
  resampled_rate_ = rate;
  ResetGate();
  return true;
}

void SqueakCaptureProcessor::Denoise48(float* frame) {
  float lsnr = 0;
  if (deep_filter_.Process(frame, &lsnr)) {
    stat_denoised_.fetch_add(1);
    stat_deep_filtered_.fetch_add(1);
    stat_lsnr_milli_.fetch_add(static_cast<int64_t>(lsnr * 1000.0f));
    ResetGate();
    return;
  }
  if (state_ == nullptr || state_->denoiser == nullptr) return;
  // Шкала совпадает: APM держит float в диапазоне int16, того же ждёт и
  // RNNoise. Обрабатываем на месте, in и out могут совпадать.
  const float vad = rnnoise_process_frame(state_->denoiser, frame, frame);
  stat_denoised_.fetch_add(1);
  stat_vad_milli_.fetch_add(static_cast<int64_t>(vad * 1000.0f));
  ApplyGate(vad, kRnnoiseFrameSize, frame);
}

SqueakCaptureProcessor::Stats SqueakCaptureProcessor::TakeStats() {
  Stats stats;
  stats.enabled = rnnoise_enabled_.load();
  stats.sample_rate_hz = sample_rate_hz_;
  stats.frames = stat_frames_.exchange(0);
  stats.denoised = stat_denoised_.exchange(0);
  stats.gated = stat_gated_.exchange(0);
  const int64_t vad_milli = stat_vad_milli_.exchange(0);
  stats.deep_filter = deep_filter_.status();
  stats.deep_filtered = stat_deep_filtered_.exchange(0);
  const int64_t lsnr_milli = stat_lsnr_milli_.exchange(0);
  const int64_t rnnoised = stats.denoised - stats.deep_filtered;
  if (rnnoised > 0) stats.vad_mean = vad_milli / 1000.0 / rnnoised;
  if (stats.deep_filtered > 0) {
    stats.lsnr_mean = lsnr_milli / 1000.0 / stats.deep_filtered;
  }

  stats.frame_us_max = stat_us_max_.exchange(0);
  int64_t buckets[kUsBuckets];
  int64_t total = 0;
  for (int i = 0; i < kUsBuckets; ++i) {
    buckets[i] = stat_us_bucket_[i].exchange(0);
    total += buckets[i];
  }
  if (total > 0) {
    const int64_t target = (total * 99 + 99) / 100;
    int64_t counted = 0;
    for (int i = 0; i < kUsBuckets; ++i) {
      counted += buckets[i];
      if (counted < target) continue;
      stats.frame_us_p99 =
          i + 1 < kUsBuckets ? kUsBucketUpper[i] : stats.frame_us_max;
      break;
    }
  }
  return stats;
}

void SqueakCaptureProcessor::RecordFrameTime(int64_t us) {
  int bucket = kUsBuckets - 1;
  for (int i = 0; i + 1 < kUsBuckets; ++i) {
    if (us < kUsBucketUpper[i]) {
      bucket = i;
      break;
    }
  }
  stat_us_bucket_[bucket].fetch_add(1);
  const int clipped = static_cast<int>(std::min<int64_t>(us, 1 << 30));
  int seen = stat_us_max_.load();
  while (clipped > seen &&
         !stat_us_max_.compare_exchange_weak(seen, clipped)) {
  }
}

void SqueakCaptureProcessor::ResetGate() {
  hold_frames_ = 0;
  gate_gain_ = 1.0f;
}

void SqueakCaptureProcessor::ApplyGate(float vad, int num_frames,
                                       float* buffer) {
  if (vad >= kVadOpen) {
    hold_frames_ = kHoldFrames;
  } else if (hold_frames_ > 0) {
    --hold_frames_;
  }

  const float target = hold_frames_ > 0 ? 1.0f : 0.0f;
  if (target == 0.0f) stat_gated_.fetch_add(1);
  if (target == 1.0f && gate_gain_ == 1.0f) return;

  const float from = gate_gain_;
  const int ramp = target > from ? kOpenRampSamples : num_frames;
  for (int i = 0; i < num_frames; ++i) {
    const float k = i < ramp ? static_cast<float>(i + 1) / ramp : 1.0f;
    buffer[i] *= from + (target - from) * k;
  }
  gate_gain_ = target;
}

void SqueakCaptureProcessor::Process(int num_bands, int num_frames,
                                     int buffer_size, float* buffer) {
  // Постпроцессор APM зовётся после слияния полос, и buffer — это полный
  // канал: num_frames сэмплов на исходной частоте. num_bands и buffer_size
  // описывают разбиение, которое к этому моменту уже собрано обратно: на
  // 48 кГц полос всегда три, и прежняя проверка «одна полоса» не пускала
  // RNNoise ни разу, а усиление писало втрое дальше конца канала.
  (void)num_bands;
  (void)buffer_size;

  if (buffer == nullptr || num_frames <= 0) return;

  stat_frames_.fetch_add(1);

  const bool monitoring = monitor_on_.load(std::memory_order_acquire) &&
                          num_frames <= kMonitorFrameMax;
  if (monitoring) std::copy_n(buffer, num_frames, monitor_input_);

  if (!rnnoise_enabled_.load()) {
    ResetGate();
  } else {
    const auto started = std::chrono::steady_clock::now();
    if (num_frames == kRnnoiseFrameSize && sample_rate_hz_ == kRnnoiseRate) {
      Denoise48(buffer);
    } else if (EnsureResamplers(num_frames)) {
      // APM отдаёт кадры на частоте записи: 16 кГц у Bluetooth-гарнитур и у
      // встроенного AEC Windows. Сети обучены на 48 кГц: поднимаем кадр,
      // чистим и возвращаем обратно.
      up_->Process(buffer, num_frames, frame48_);
      Denoise48(frame48_);
      down_->Process(frame48_, kRnnoiseFrameSize, buffer);
    } else if (!warned_) {
      warned_ = true;
      std::fprintf(stderr,
                   "[squeak] шумодав: пропуск, %d сэмплов за 10 мс не "
                   "пересчитываются в 48 кГц\n",
                   num_frames);
    }
    RecordFrameTime(std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - started)
                        .count());
  }

  const float gain = gain_.load();
  if (gain != 1.0f) {
    for (int i = 0; i < num_frames; ++i) {
      buffer[i] = std::clamp(buffer[i] * gain, -kSampleMax, kSampleMax);
    }
  }

  if (monitoring) MonitorWrite(monitor_input_, buffer, num_frames);
}

void SqueakCaptureProcessor::StartMonitor(int seconds) {
  const int rate = sample_rate_hz_.load();
  auto monitor = std::make_unique<Monitor>();
  monitor->sample_rate_hz = rate;
  monitor->capacity =
      static_cast<size_t>(std::clamp(seconds, 1, 30)) * (rate > 0 ? rate : 48000);
  monitor->before.assign(monitor->capacity, 0.0f);
  monitor->after.assign(monitor->capacity, 0.0f);
  {
    std::lock_guard<std::mutex> lock(monitor_mutex_);
    monitor_ = std::move(monitor);
  }
  monitor_on_.store(true, std::memory_order_release);
}

SqueakCaptureProcessor::MonitorTake SqueakCaptureProcessor::StopMonitor() {
  monitor_on_.store(false);
  std::unique_ptr<Monitor> monitor;
  {
    std::lock_guard<std::mutex> lock(monitor_mutex_);
    monitor = std::move(monitor_);
  }
  MonitorTake take;
  if (monitor == nullptr) return take;
  take.sample_rate_hz = monitor->sample_rate_hz;
  // Разворачиваем кольцо в хронологический порядок.
  const size_t filled = monitor->wrapped ? monitor->capacity : monitor->pos;
  const size_t start = monitor->wrapped ? monitor->pos : 0;
  take.before.reserve(filled);
  take.after.reserve(filled);
  for (size_t i = 0; i < filled; ++i) {
    const size_t idx = (start + i) % monitor->capacity;
    take.before.push_back(monitor->before[idx]);
    take.after.push_back(monitor->after[idx]);
  }
  return take;
}

void SqueakCaptureProcessor::MonitorWrite(const float* before,
                                          const float* after, int num_frames) {
  std::unique_lock<std::mutex> lock(monitor_mutex_, std::try_to_lock);
  if (!lock.owns_lock() || monitor_ == nullptr) return;
  Monitor& m = *monitor_;
  // Смена частоты посреди записи — начинаем кольцо заново на новой частоте.
  const int rate = sample_rate_hz_.load();
  if (rate != m.sample_rate_hz && rate > 0) {
    m.sample_rate_hz = rate;
    m.pos = 0;
    m.wrapped = false;
  }
  for (int i = 0; i < num_frames; ++i) {
    m.before[m.pos] = before[i];
    m.after[m.pos] = after[i];
    if (++m.pos == m.capacity) {
      m.pos = 0;
      m.wrapped = true;
    }
  }
}

}  // namespace flutter_webrtc_plugin
