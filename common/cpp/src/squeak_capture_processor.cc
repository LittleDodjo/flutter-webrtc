#include "squeak_capture_processor.h"

#include <algorithm>
#include <cstdio>

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

bool SqueakCaptureProcessor::RnnoiseApplicable(int num_frames) const {
  return num_frames == kRnnoiseFrameSize && sample_rate_hz_ == kRnnoiseRate;
}

SqueakCaptureProcessor::Stats SqueakCaptureProcessor::TakeStats() {
  Stats stats;
  stats.enabled = rnnoise_enabled_.load();
  stats.sample_rate_hz = sample_rate_hz_;
  stats.frames = stat_frames_.exchange(0);
  stats.denoised = stat_denoised_.exchange(0);
  stats.gated = stat_gated_.exchange(0);
  const int64_t vad_milli = stat_vad_milli_.exchange(0);
  if (stats.denoised > 0) {
    stats.vad_mean = vad_milli / 1000.0 / stats.denoised;
  }
  return stats;
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

  if (!rnnoise_enabled_.load()) {
    ResetGate();
  } else if (state_ != nullptr && state_->denoiser != nullptr) {
    if (RnnoiseApplicable(num_frames)) {
      // Шкала совпадает: APM держит float в диапазоне int16, того же ждёт и
      // RNNoise. Обрабатываем на месте, in и out могут совпадать.
      const float vad =
          rnnoise_process_frame(state_->denoiser, buffer, buffer);
      stat_denoised_.fetch_add(1);
      stat_vad_milli_.fetch_add(static_cast<int64_t>(vad * 1000.0f));
      ApplyGate(vad, num_frames, buffer);
    } else if (!warned_) {
      warned_ = true;
      std::fprintf(stderr,
                   "[squeak] rnnoise: пропуск, ожидалось 480 кадров "
                   "на 48000 Гц, пришло %d на %d Гц\n",
                   num_frames, sample_rate_hz_);
    }
  }

  const float gain = gain_.load();
  if (gain == 1.0f) return;

  for (int i = 0; i < num_frames; ++i) {
    buffer[i] = std::clamp(buffer[i] * gain, -kSampleMax, kSampleMax);
  }
}

}  // namespace flutter_webrtc_plugin
