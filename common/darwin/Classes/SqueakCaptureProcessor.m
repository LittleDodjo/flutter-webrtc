#import "SqueakCaptureProcessor.h"

#import <stdatomic.h>

// APM держит float в шкале int16. Выход за неё уводит сэмплы в клиппинг уже
// при кодировании, поэтому подрезаем сами.
static const float kSqueakSampleMax = 32767.0f;

@implementation SqueakCaptureProcessor {
  _Atomic float _gain;
}

- (instancetype)init {
  if (self = [super init]) {
    atomic_store(&_gain, 1.0f);
  }
  return self;
}

- (float)gain {
  return atomic_load(&_gain);
}

- (void)setGain:(float)gain {
  atomic_store(&_gain, gain);
}

- (void)audioProcessingInitializeWithSampleRate:(size_t)sampleRateHz channels:(size_t)channels {
}

- (void)audioProcessingProcess:(RTC_OBJC_TYPE(RTCAudioBuffer) * _Nonnull)audioBuffer {
  const float gain = atomic_load(&_gain);
  if (gain == 1.0f) {
    return;
  }

  // frames — это все полосы канала подряд, поэтому усиление ложится на весь
  // кадр разом: линейное масштабирование переживает разбиение по полосам.
  for (size_t channel = 0; channel < audioBuffer.channels; channel++) {
    float* samples = [audioBuffer rawBufferForChannel:channel];
    for (size_t i = 0; i < audioBuffer.frames; i++) {
      const float value = samples[i] * gain;
      samples[i] = value > kSqueakSampleMax
                       ? kSqueakSampleMax
                       : (value < -kSqueakSampleMax ? -kSqueakSampleMax : value);
    }
  }
}

- (void)audioProcessingRelease {
}

@end
