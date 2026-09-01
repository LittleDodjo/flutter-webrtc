#ifndef FLUTTER_WEBRTC_SQUEAK_CAPTURE_PROCESSOR_HXX
#define FLUTTER_WEBRTC_SQUEAK_CAPTURE_PROCESSOR_HXX

#include <atomic>
#include <memory>

#include "rtc_audio_processing.h"

namespace flutter_webrtc_plugin {

// Наша врезка в цепочку захвата, сразу за штатным APM. Делает две вещи:
// нейросетевой шумодав Xiph RNNoise и усиление микрофона.
//
// Усиление живёт именно здесь, потому что штатного пути нет: SetVolume на
// локальном треке уходит в LocalAudioSource::SetVolume, а он в libwebrtc
// пустой — громкость умеет только удалённый источник. Отсюда и «поставил
// 200% и ничего не изменилось».
//
// Порядок: сначала шумодав, потом усиление. Наоборот сеть увидела бы уже
// задранный фон.
class SqueakCaptureProcessor
    : public libwebrtc::RTCAudioProcessing::CustomProcessing {
 public:
  SqueakCaptureProcessor();
  ~SqueakCaptureProcessor() override;

  void SetRnnoiseEnabled(bool enabled) { rnnoise_enabled_.store(enabled); }

  // 1.0 — как есть. Ниже — тише, выше — громче, с ограничением по пику.
  void SetGain(float gain) { gain_.store(gain); }

  void Initialize(int sample_rate_hz, int num_channels) override;

  void Process(int num_bands, int num_frames, int buffer_size,
               float* buffer) override;

  void Reset(int new_rate) override;

  void Release() override;

 private:
  struct State;

  // RNNoise работает только на 48 кГц кадрами по 480 сэмплов. На другой
  // частоте шумодав пропускаем, а усиление применяем всё равно.
  bool RnnoiseApplicable(int num_bands, int num_frames) const;

  std::atomic<bool> rnnoise_enabled_{false};

  std::atomic<float> gain_{1.0f};

  std::unique_ptr<State> state_;

  int sample_rate_hz_ = 0;

  bool warned_ = false;
};

}  // namespace flutter_webrtc_plugin

#endif  // FLUTTER_WEBRTC_SQUEAK_CAPTURE_PROCESSOR_HXX
