#ifndef FLUTTER_WEBRTC_SQUEAK_RNNOISE_PROCESSOR_HXX
#define FLUTTER_WEBRTC_SQUEAK_RNNOISE_PROCESSOR_HXX

#include <atomic>
#include <memory>
#include <vector>

#include "rtc_audio_processing.h"

namespace flutter_webrtc_plugin {

// Нейросетевой шумодав Xiph RNNoise поверх штатного APM. Ставится
// capture-постпроцессором, то есть работает уже после эхоподавителя: эхо и шум
// — разные задачи, и подменять AEC он не должен.
//
// Живёт весь сеанс, включается флагом: пересоздавать состояние на каждом
// переключении значит терять адаптацию сети к голосу.
class RnnoiseProcessor : public libwebrtc::RTCAudioProcessing::CustomProcessing {
 public:
  RnnoiseProcessor();
  ~RnnoiseProcessor() override;

  void SetEnabled(bool enabled) { enabled_.store(enabled); }

  bool enabled() const { return enabled_.load(); }

  void Initialize(int sample_rate_hz, int num_channels) override;

  void Process(int num_bands, int num_frames, int buffer_size,
               float* buffer) override;

  void Reset(int new_rate) override;

  void Release() override;

 private:
  struct State;

  // RNNoise работает только на 48 кГц кадрами по 480 сэмплов. На другой
  // частоте пропускаем звук нетронутым, а не портим его.
  bool Applicable(int num_bands, int num_frames) const;

  std::atomic<bool> enabled_{false};

  std::unique_ptr<State> state_;

  int sample_rate_hz_ = 0;

  bool warned_ = false;
};

}  // namespace flutter_webrtc_plugin

#endif  // FLUTTER_WEBRTC_SQUEAK_RNNOISE_PROCESSOR_HXX
