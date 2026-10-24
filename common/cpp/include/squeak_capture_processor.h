#ifndef FLUTTER_WEBRTC_SQUEAK_CAPTURE_PROCESSOR_HXX
#define FLUTTER_WEBRTC_SQUEAK_CAPTURE_PROCESSOR_HXX

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "rtc_audio_processing.h"
#include "squeak_deep_filter.h"
#include "squeak_resampler.h"

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
    // Время шумодава на кадр 10 мс: p99 по корзинам (верхняя граница корзины)
    // и максимум. Кадр дольше 10 мс в потоке захвата — это заикание.
    int frame_us_p99 = 0;
    int frame_us_max = 0;
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

  const SqueakDeepFilter& deep_filter() const { return deep_filter_; }

  // Запись последних [seconds] секунд звука до врезки и после неё, на частоте
  // кадров APM. Для кнопки «Проверить шумодав»: человек слушает обе записи
  // на своей машине. Повторный Start сбрасывает буферы.
  void StartMonitor(int seconds);

  struct MonitorTake {
    int sample_rate_hz = 0;
    std::vector<float> before;
    std::vector<float> after;
  };

  // Останавливает запись и отдаёт её; пусто, если запись не шла.
  MonitorTake StopMonitor();

  void Initialize(int sample_rate_hz, int num_channels) override;

  void Process(int num_bands, int num_frames, int buffer_size,
               float* buffer) override;

  void Reset(int new_rate) override;

  void Release() override;

 private:
  struct State;

  // Обе сети работают на 48 кГц кадрами по 480 сэмплов. Другую частоту APM
  // (16 и 32 кГц) пересчитываем; не пересчитывается — шумодав пропускаем, а
  // усиление применяем всё равно.
  bool EnsureResamplers(int num_frames);

  // Кадр 480 сэмплов на 48 кГц: DFN3, если готов, иначе RNNoise с гейтом.
  void Denoise48(float* frame);

  // Гасит кадр, если голоса нет дольше удержания. Плавно, чтобы на границе
  // не было щелчка от самого гейта.
  void ApplyGate(float vad, int num_frames, float* buffer);

  void ResetGate();

  void RecordFrameTime(int64_t us);

  // Кольцевые буферы монитора. Аудиопоток пишет под try_lock: если платформенный
  // поток как раз забирает запись, кадр пропускается, но поток не ждёт.
  struct Monitor {
    int sample_rate_hz = 0;
    size_t capacity = 0;
    size_t pos = 0;
    bool wrapped = false;
    std::vector<float> before;
    std::vector<float> after;
  };

  void MonitorWrite(const float* before, const float* after, int num_frames);

  // Корзины времени кадра, мкс: < 500, < 1000, < 2000, < 4000, < 8000, прочее.
  static constexpr int kUsBuckets = 6;

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
  std::atomic<int64_t> stat_us_bucket_[kUsBuckets] = {};
  std::atomic<int> stat_us_max_{0};

  SqueakDeepFilter deep_filter_;

  std::unique_ptr<SqueakResampler> up_;
  std::unique_ptr<SqueakResampler> down_;
  int resampled_rate_ = 0;
  float frame48_[480] = {};

  std::atomic<float> gain_{1.0f};

  std::atomic<bool> monitor_on_{false};
  std::mutex monitor_mutex_;
  std::unique_ptr<Monitor> monitor_;
  // Копия входа кадра для монитора; без аллокаций в аудиопотоке.
  static constexpr int kMonitorFrameMax = 1920;
  float monitor_input_[kMonitorFrameMax] = {};

  std::unique_ptr<State> state_;

  // Пишет аудиопоток, читает TakeStats с платформенного.
  std::atomic<int> sample_rate_hz_{0};

  bool warned_ = false;
};

}  // namespace flutter_webrtc_plugin

#endif  // FLUTTER_WEBRTC_SQUEAK_CAPTURE_PROCESSOR_HXX
