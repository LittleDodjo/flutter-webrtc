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

  if (rnnoise_enabled_.load() && state_ != nullptr &&
      state_->denoiser != nullptr) {
    if (RnnoiseApplicable(num_frames)) {
      // Шкала совпадает: APM держит float в диапазоне int16, того же ждёт и
      // RNNoise. Обрабатываем на месте, in и out могут совпадать.
      rnnoise_process_frame(state_->denoiser, buffer, buffer);
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
