#import <Foundation/Foundation.h>
#import <WebRTC/WebRTC.h>

#import "AudioProcessingAdapter.h"

// Врезка в цепочку захвата сразу за штатным APM: усиление микрофона.
// Штатного пути нет — SetVolume на локальном треке уходит в
// LocalAudioSource::SetVolume, а он в libwebrtc пустой. Отсюда и «поставил
// 200% и ничего не изменилось».
//
// Шумодава здесь нет: RNNoise вендорится только в Windows- и Linux-сборках.
@interface SqueakCaptureProcessor : NSObject <ExternalAudioProcessingDelegate>

// 1.0 — как есть. Ниже — тише, выше — громче, с ограничением по пику.
@property(nonatomic) float gain;

@end
