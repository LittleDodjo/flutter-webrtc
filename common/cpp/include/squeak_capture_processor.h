#ifndef FLUTTER_WEBRTC_SQUEAK_CAPTURE_PROCESSOR_HXX
#define FLUTTER_WEBRTC_SQUEAK_CAPTURE_PROCESSOR_HXX

#include <atomic>
#include <cstdint>
#include <memory>

#include "rtc_audio_processing.h"
#include "squeak_deep_filter.h"

namespace flutter_webrtc_plugin {

// Наша врезка в цепочку захвата, сразу за штатным APM (после его шумодава,
// эхоподавителя и AGC2 — отдельного места «до APM» у постпроцессора нет). Делает две вещи:
// нейросетевой шумодав и усиление микрофона.
//
// Шумодав — DeepFilterNet3, если его библиотека рядом и модель поднялась, иначе
// Xiph RNNoise с гейтом. На речи поверх механической клавиатуры DFN3 оставляет
// в паузах −71 dBFS против −48 у RNNoise с гейтом, а речь портит меньше
// (SI-SDR 19 дБ против 14); цена — 30 мс задержки вместо 10 и ~0,6 мс на кадр.
//
// Усиление живёт именно здесь, потому что штатного пути нет: SetVolume на
// локальном треке уходит в LocalAudioSource::SetVolume, а он в libwebrtc
// пустой — громкость умеет только удалённый источник. Отсюда и «поставил
// 200% и ничего не изменилось».
//
// Порядок: сначала шумодав, потом гейт, потом усиление. Наоборот сеть увидела
// бы уже задранный фон.
//
// Гейт — только для RNNoise: DFN3 щелчки в паузах убирает сам. Он закрывает
// кадры, где RNNoise не слышит голоса. Сам RNNoise короткие
// щелчки (клавиатура, мышь) только приглушает: модель обучена на стационарном
// шуме, а щелчок для неё похож на согласную. Зато его вероятность голоса на
// таких кадрах низкая, и в паузах между словами щелчки уходят целиком.
class SqueakCaptureProcessor
    : public libwebrtc::RTCAudioProcessing::CustomProcessing {
 public:
  // Снимок счётчиков с прошлого снятия: по нему клиент пишет в лог, работает
  // ли шумодав на самом деле.
  struct Stats {
    bool enabled = false;
    int sample_rate_hz = 0;
    int64_t frames = 0;
    int64_t denoised = 0;
    int64_t gated = 0;
    double vad_mean = 0;
    SqueakDeepFilter::Status deep_filter = SqueakDeepFilter::Status::kIdle;
    // Кадры через DFN3 (входят и в denoised) и их средний локальный SNR, дБ.
    int64_t deep_filtered = 0;
    double lsnr_mean = 0;
  };

  SqueakCaptureProcessor();
  ~SqueakCaptureProcessor() override;

  void SetRnnoiseEnabled(bool enabled) {
    if (enabled) deep_filter_.Prepare();
    rnnoise_enabled_.store(enabled);
  }

  // Снимает счётчики и обнуляет их. Читатель один — периодическая строка
  // статистики звука в клиенте.
  Stats TakeStats();

  // 1.0 — как есть. Ниже — тише, выше — громче, с ограничением по пику.
  void SetGain(float gain) { gain_.store(gain); }

  void Initialize(int sample_rate_hz, int num_channels) override;

  void Process(int num_bands, int num_frames, int buffer_size,
               float* buffer) override;

  void Reset(int new_rate) override;

  void Release() override;

 private:
  struct State;

  // Оба шумодава работают только на 48 кГц кадрами по 480 сэмплов. На другой
  // частоте шумодав пропускаем, а усиление применяем всё равно.
  bool RnnoiseApplicable(int num_frames) const;

  // Гасит кадр, если голоса нет дольше удержания. Плавно, чтобы на границе
  // не было щелчка от самого гейта.
  void ApplyGate(float vad, int num_frames, float* buffer);

  void ResetGate();

  std::atomic<bool> rnnoise_enabled_{false};

  // Сколько кадров по 10 мс гейт ещё держит открытым после последнего голоса.
  int hold_frames_ = 0;

  // Текущий множитель гейта: 1 — открыт, 0 — закрыт.
  float gate_gain_ = 1.0f;

  std::atomic<int64_t> stat_frames_{0};
  std::atomic<int64_t> stat_denoised_{0};
  std::atomic<int64_t> stat_gated_{0};
  // Сумма вероятностей голоса в тысячных: атомарного double нет.
  std::atomic<int64_t> stat_vad_milli_{0};
  std::atomic<int64_t> stat_deep_filtered_{0};
  std::atomic<int64_t> stat_lsnr_milli_{0};

  SqueakDeepFilter deep_filter_;

  std::atomic<float> gain_{1.0f};

  std::unique_ptr<State> state_;

  int sample_rate_hz_ = 0;

  bool warned_ = false;
};

}  // namespace flutter_webrtc_plugin

#endif  // FLUTTER_WEBRTC_SQUEAK_CAPTURE_PROCESSOR_HXX
