#ifndef FLUTTER_WEBRTC_SQUEAK_DEEP_FILTER_HXX
#define FLUTTER_WEBRTC_SQUEAK_DEEP_FILTER_HXX

#include <atomic>
#include <cstddef>
#include <mutex>
#include <thread>

namespace flutter_webrtc_plugin {

// Шумодав DeepFilterNet3 из squeak_df.dll / libsqueak_df.so рядом с плагином
// (third_party/squeak_df). Библиотеку не линкуем, а грузим: нет её или модель
// не поднялась — захват идёт через RNNoise, а не падает.
//
// Модель поднимается ~0,2 с, поэтому в отдельном потоке при первом включении
// шумодава. До готовности кадры идут мимо.
class SqueakDeepFilter {
 public:
  enum class Status { kIdle, kLoading, kReady, kMissing, kFailed };

  // 10 мс на 48 кГц — ровно кадр APM.
  static constexpr int kFrameSize = 480;

  SqueakDeepFilter() = default;
  ~SqueakDeepFilter();

  SqueakDeepFilter(const SqueakDeepFilter&) = delete;
  SqueakDeepFilter& operator=(const SqueakDeepFilter&) = delete;

  // Запускает загрузку, если её ещё не было. Из любого потока.
  void Prepare();

  Status status() const { return status_.load(); }

  // Кадр kFrameSize сэмплов в шкале int16, на месте. Только из аудиопотока и
  // только при kReady. Возвращает локальный SNR кадра в дБ; false — модель
  // сломалась, кадр не тронут, дальше статус kFailed.
  bool Process(float* buffer, float* lsnr);

 private:
  using CreateFn = void* (*)(float, float);
  using ProcessFn = float (*)(void*, const float*, float*);
  using FreeFn = void (*)(void*);
  using FrameLengthFn = size_t (*)(const void*);

  void Load();

  std::once_flag once_;
  std::thread loader_;
  std::atomic<Status> status_{Status::kIdle};

  void* state_ = nullptr;
  ProcessFn process_ = nullptr;
  FreeFn free_ = nullptr;

  float in_[kFrameSize] = {};
  float out_[kFrameSize] = {};
};

}  // namespace flutter_webrtc_plugin

#endif  // FLUTTER_WEBRTC_SQUEAK_DEEP_FILTER_HXX
