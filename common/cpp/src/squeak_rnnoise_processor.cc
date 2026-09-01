#include "squeak_rnnoise_processor.h"

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

}  // namespace

struct RnnoiseProcessor::State {
  DenoiseState* denoiser = nullptr;

  ~State() {
    if (denoiser != nullptr) rnnoise_destroy(denoiser);
  }
};

RnnoiseProcessor::RnnoiseProcessor() = default;

RnnoiseProcessor::~RnnoiseProcessor() = default;

void RnnoiseProcessor::Initialize(int sample_rate_hz, int num_channels) {
  sample_rate_hz_ = sample_rate_hz;
  warned_ = false;
  if (state_ == nullptr) {
    state_ = std::make_unique<State>();
    state_->denoiser = rnnoise_create(nullptr);
  }
}

void RnnoiseProcessor::Reset(int new_rate) {
  sample_rate_hz_ = new_rate;
  warned_ = false;
}

void RnnoiseProcessor::Release() {
  state_.reset();
  sample_rate_hz_ = 0;
}

bool RnnoiseProcessor::Applicable(int num_bands, int num_frames) const {
  return num_bands == 1 && num_frames == kRnnoiseFrameSize &&
         sample_rate_hz_ == kRnnoiseRate;
}

void RnnoiseProcessor::Process(int num_bands, int num_frames, int buffer_size,
                               float* buffer) {
  // buffer_size приходит как kNsFrameSize * num_bands и с реальной длиной
  // кадра не совпадает — считаем по num_frames.
  (void)buffer_size;

  if (!enabled_.load() || buffer == nullptr) return;
  if (state_ == nullptr || state_->denoiser == nullptr) return;

  if (!Applicable(num_bands, num_frames)) {
    if (!warned_) {
      warned_ = true;
      std::fprintf(stderr,
                   "[squeak] rnnoise: пропуск, ожидалось 1 полоса/480 кадров "
                   "на 48000 Гц, пришло %d/%d на %d Гц\n",
                   num_bands, num_frames, sample_rate_hz_);
    }
    return;
  }

  // Шкала совпадает: APM держит float в диапазоне int16, и того же ждёт
  // RNNoise. Обрабатываем на месте, in и out могут совпадать.
  rnnoise_process_frame(state_->denoiser, buffer, buffer);
}

}  // namespace flutter_webrtc_plugin
