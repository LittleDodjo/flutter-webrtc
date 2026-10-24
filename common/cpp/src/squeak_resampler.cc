#include "squeak_resampler.h"

#include <algorithm>
#include <cmath>

namespace flutter_webrtc_plugin {
namespace {

// Отводов на фазу: ~1 мс задержки в каждую сторону на 16 кГц.
constexpr int kTapsPerPhase = 32;

// Окно Кайзера с β = 8: подавление за полосой ~80 дБ.
constexpr double kKaiserBeta = 8.0;

// Срез чуть ниже половины меньшей частоты, чтобы переходная полоса не
// заворачивала спектр.
constexpr double kCutoffMargin = 0.94;

constexpr double kPi = 3.14159265358979323846;

double BesselI0(double x) {
  double sum = 1.0, term = 1.0;
  for (int k = 1; k < 50; ++k) {
    term *= (x / (2.0 * k)) * (x / (2.0 * k));
    sum += term;
    if (term < 1e-12 * sum) break;
  }
  return sum;
}

}  // namespace

SqueakResampler::SqueakResampler(int up, int down) : up_(up), down_(down) {
  const int factor = std::max(up, down);
  // Нечётная длина: пара «вверх-вниз» задерживает ровно на целое число
  // сэмплов исходной частоты, а не на дробь.
  const int n = kTapsPerPhase * factor + 1;
  // Частота среза в долях частоты после повышения, с усилением up_: вставка
  // нулей делит амплитуду на up_.
  const double fc = 0.5 / factor * kCutoffMargin;
  const double mid = (n - 1) / 2.0;
  const double norm = BesselI0(kKaiserBeta);
  taps_.resize(n);
  for (int i = 0; i < n; ++i) {
    const double x = i - mid;
    const double sinc =
        x == 0 ? 2 * fc : std::sin(2 * kPi * fc * x) / (kPi * x);
    const double r = 2.0 * i / (n - 1) - 1.0;
    const double window = BesselI0(kKaiserBeta * std::sqrt(1 - r * r)) / norm;
    taps_[i] = static_cast<float>(up * sinc * window);
  }
  Reset();
}

void SqueakResampler::Reset() {
  const int keep = static_cast<int>(taps_.size()) / up_ + 1;
  history_.assign(keep, 0.0f);
  // Запас под кадр 10 мс на 48 кГц: в аудиопотоке без перевыделений.
  history_.reserve(keep + 512);
  consumed_ = keep;
  next_t_ = static_cast<int64_t>(keep) * up_;
}

int SqueakResampler::Process(const float* in, int n, float* out) {
  history_.insert(history_.end(), in, in + n);
  consumed_ += n;
  const int64_t base = consumed_ - static_cast<int64_t>(history_.size());
  const int64_t ntaps = static_cast<int64_t>(taps_.size());

  // Отсчёт t сетки после повышения доступен, пока t < consumed_·up_: нули,
  // вставленные после последнего входного сэмпла, известны заранее.
  int written = 0;
  const int64_t limit = consumed_ * up_;
  for (; next_t_ < limit; next_t_ += down_) {
    const int64_t i_max = next_t_ / up_;
    double acc = 0;
    for (int64_t i = i_max; i >= base; --i) {
      const int64_t k = next_t_ - i * up_;
      if (k >= ntaps) break;
      acc += taps_[k] * history_[i - base];
    }
    out[written++] = static_cast<float>(acc);
  }

  const size_t keep = taps_.size() / up_ + 1;
  if (history_.size() > keep) {
    history_.erase(history_.begin(), history_.end() - keep);
  }
  return written;
}

}  // namespace flutter_webrtc_plugin
