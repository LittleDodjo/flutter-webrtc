#ifndef FLUTTER_WEBRTC_SQUEAK_RESAMPLER_HXX
#define FLUTTER_WEBRTC_SQUEAK_RESAMPLER_HXX

#include <cstdint>
#include <vector>

namespace flutter_webrtc_plugin {

// Потоковый ресемплер с рациональным шагом L/M (полифазный FIR, окно Кайзера).
// Нужен шумодавам: обе сети работают на 48 кГц, а APM обрабатывает на частоте
// микрофона — у гарнитур это нередко 16 кГц. За вызов на N входных сэмплов
// отдаёт ровно N·L/M (кадры по 10 мс делятся нацело).
class SqueakResampler {
 public:
  SqueakResampler(int up, int down);

  int up() const { return up_; }
  int down() const { return down_; }

  // Возвращает число записанных в out сэмплов.
  int Process(const float* in, int n, float* out);

  void Reset();

 private:
  int up_;
  int down_;
  std::vector<float> taps_;
  // Хвост прошлых входных сэмплов и новые — подряд.
  std::vector<float> history_;
  int64_t consumed_ = 0;
  int64_t next_t_ = 0;
};

}  // namespace flutter_webrtc_plugin

#endif  // FLUTTER_WEBRTC_SQUEAK_RESAMPLER_HXX
