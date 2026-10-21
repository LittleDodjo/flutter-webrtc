#import "FlutterScreenCaptureKitCapturer.h"

#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>

#if __has_include(<ScreenCaptureKit/ScreenCaptureKit.h>)
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#endif

// ScreenCaptureKit не отдаёт кадр, пока картинка не менялась: статичный стол
// при 60 к/с даёт единицы кадров в секунду, и webrtc считает поток
// остановившимся (а зритель видит замершую метку fps). Повторяем последний
// кадр сами, если новый не пришёл за два периода.
static const double kRepeatAfterPeriods = 2.0;

@interface FlutterScreenCaptureKitCapturer ()
#if __has_include(<ScreenCaptureKit/ScreenCaptureKit.h>)
<SCStreamOutput, SCStreamDelegate>
#endif
@property(nonatomic, strong) RTCVideoCapturer *capturer;
@property(nonatomic, weak) id<RTCVideoCapturerDelegate> delegate;
@property(nonatomic, strong) dispatch_queue_t captureQueue;
@property(nonatomic, strong) dispatch_source_t repeatTimer;
@property(nonatomic, assign) CVPixelBufferRef lastPixelBuffer;
@property(nonatomic, assign) int64_t lastFrameHostNs;
@property(nonatomic, assign) int64_t periodNs;
@property(nonatomic, assign) NSInteger droppedLogged;
#if __has_include(<ScreenCaptureKit/ScreenCaptureKit.h>)
@property(nonatomic, strong) SCStream *stream API_AVAILABLE(macos(12.3));
#endif
@end

@implementation FlutterScreenCaptureKitCapturer

- (instancetype)initWithDelegate:(id<RTCVideoCapturerDelegate>)delegate {
  self = [super init];
  if (self) {
    _delegate = delegate;
    _capturer = [[RTCVideoCapturer alloc] initWithDelegate:delegate];
    // Кадры 4K60 нельзя терять на очереди с обычным приоритетом: UI и сеть
    // отодвинули бы захват.
    dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(
        DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
    _captureQueue = dispatch_queue_create("ru.squeak.sck.capture", attr);
  }
  return self;
}

- (void)dealloc {
  [self stopRepeatTimer];
  if (_lastPixelBuffer) {
    CVPixelBufferRelease(_lastPixelBuffer);
    _lastPixelBuffer = NULL;
  }
}

- (void)startCaptureWithFPS:(NSInteger)fps
                   sourceId:(NSString* _Nullable)sourceId
                  onStarted:(void (^)(NSError * _Nullable error))onStarted {
  [self startCaptureWithFPS:fps
                targetWidth:0
               targetHeight:0
                   sourceId:sourceId
                  onStarted:onStarted];
}

- (void)startCaptureWithFPS:(NSInteger)fps
                targetWidth:(NSInteger)targetWidth
               targetHeight:(NSInteger)targetHeight
                   sourceId:(NSString* _Nullable)sourceId
                  onStarted:(void (^)(NSError * _Nullable error))onStarted {
#if __has_include(<ScreenCaptureKit/ScreenCaptureKit.h>)
  if (@available(macOS 12.3, *)) {
    [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent *content, NSError *error) {
      if (error != nil) {
        onStarted(error);
        return;
      }

      SCDisplay *display = [self selectDisplayFromContent:content sourceId:sourceId];
      if (display == nil) {
        NSError *noDisplay = [NSError errorWithDomain:@"FlutterScreenCaptureKit"
                                                 code:-1
                                             userInfo:@{NSLocalizedDescriptionKey: @"No matching display"}];
        onStarted(noDisplay);
        return;
      }

      SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
      SCStreamConfiguration *config = [SCStreamConfiguration new];

      // display.width/height — пойнты. На Retina это половина пикселей, и
      // честного 4K (да и 1080p с 1440p-экрана) так не получить. Берём
      // физический размер режима экрана, а потом вписываем его в запрошенный
      // габарит: масштабирует сам ScreenCaptureKit на видеокарте, вместо
      // cropAndScale на процессоре внутри webrtc.
      CGDirectDisplayID displayID = display.displayID;
      size_t pixelWidth = CGDisplayPixelsWide(displayID);
      size_t pixelHeight = CGDisplayPixelsHigh(displayID);
      CGDisplayModeRef mode = CGDisplayCopyDisplayMode(displayID);
      if (mode != NULL) {
        pixelWidth = CGDisplayModeGetPixelWidth(mode);
        pixelHeight = CGDisplayModeGetPixelHeight(mode);
        CGDisplayModeRelease(mode);
      }
      if (pixelWidth == 0 || pixelHeight == 0) {
        pixelWidth = (size_t)display.width;
        pixelHeight = (size_t)display.height;
      }

      size_t outWidth = pixelWidth;
      size_t outHeight = pixelHeight;
      if (targetWidth > 0 && targetHeight > 0 &&
          (pixelWidth > (size_t)targetWidth || pixelHeight > (size_t)targetHeight)) {
        double scale = MIN((double)targetWidth / (double)pixelWidth,
                           (double)targetHeight / (double)pixelHeight);
        outWidth = (size_t)(pixelWidth * scale + 0.5);
        outHeight = (size_t)(pixelHeight * scale + 0.5);
      }
      // Чётные стороны: NV12 хранит цветность на пару пикселей, а энкодеры
      // VideoToolbox ещё и выравнивают по 2.
      outWidth &= ~(size_t)1;
      outHeight &= ~(size_t)1;
      config.width = outWidth;
      config.height = outHeight;

      // minimumFrameInterval — нижняя граница интервала, а не частота.
      // ProMotion-экран обновляется 120 раз в секунду, и при ровно 1/60 кадры
      // приходят то через 16.7 мс, то через 25 (пропуск такта) — отсюда
      // 53-56 к/с в замерах. Просим чуть чаще, чем нужно: ScreenCaptureKit
      // отдаёт каждый второй такт стабильно, а планку 60 держит webrtc.
      NSInteger clampedFps = MAX(1, MIN(fps, 240));
      config.minimumFrameInterval = CMTimeMake(1, (int32_t)(clampedFps * 2));
      self.periodNs = (int64_t)(1000000000.0 / (double)clampedFps);
      // Глубина очереди: 3 по умолчанию — при 60 к/с это 50 мс задержки под
      // стрессом и выброшенные кадры, когда энкодер держит буфер дольше кадра.
      config.queueDepth = 8;
      config.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
      config.colorSpaceName = kCGColorSpaceITUR_709;
      config.scalesToFit = YES;
      if (@available(macOS 13.0, *)) {
        config.showsCursor = YES;
      }
      if (@available(macOS 14.0, *)) {
        config.capturesShadowsOnly = NO;
        config.ignoreShadowsDisplay = NO;
      }

      NSLog(@"ScreenCaptureKit: display %u %zux%zu px -> %zux%zu @%ld (interval 1/%ld)",
            displayID, pixelWidth, pixelHeight, outWidth, outHeight, (long)clampedFps,
            (long)(clampedFps * 2));

      self.stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:self];
      NSError *addOutputError = nil;
      [self.stream addStreamOutput:self
                              type:SCStreamOutputTypeScreen
               sampleHandlerQueue:self.captureQueue
                            error:&addOutputError];
      if (addOutputError != nil) {
        onStarted(addOutputError);
        return;
      }

      [self.stream startCaptureWithCompletionHandler:^(NSError * _Nullable startError) {
        if (startError == nil) {
          [self startRepeatTimer];
        }
        onStarted(startError);
      }];
    }];
    return;
  }
#endif

  NSError *unavailable = [NSError errorWithDomain:@"FlutterScreenCaptureKit"
                                             code:-2
                                         userInfo:@{NSLocalizedDescriptionKey: @"ScreenCaptureKit not available"}];
  onStarted(unavailable);
}

- (void)stopCaptureWithCompletion:(void (^)(void))completion {
  [self stopRepeatTimer];
#if __has_include(<ScreenCaptureKit/ScreenCaptureKit.h>)
  if (@available(macOS 12.3, *)) {
    if (self.stream == nil) {
      completion();
      return;
    }
    SCStream *stream = self.stream;
    self.stream = nil;
    [stream stopCaptureWithCompletionHandler:^(__unused NSError * _Nullable error) {
      completion();
    }];
    return;
  }
#endif
  completion();
}

#pragma mark - Повтор последнего кадра

- (void)startRepeatTimer {
  [self stopRepeatTimer];
  dispatch_source_t timer =
      dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, self.captureQueue);
  uint64_t interval = (uint64_t)self.periodNs;
  dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, interval), interval,
                            interval / 4);
  __weak FlutterScreenCaptureKitCapturer *weakSelf = self;
  dispatch_source_set_event_handler(timer, ^{
    [weakSelf repeatIfStalled];
  });
  dispatch_resume(timer);
  self.repeatTimer = timer;
}

- (void)stopRepeatTimer {
  if (self.repeatTimer != nil) {
    dispatch_source_cancel(self.repeatTimer);
    self.repeatTimer = nil;
  }
}

- (void)repeatIfStalled {
  // На очереди захвата — гонки с didOutputSampleBuffer нет.
  CVPixelBufferRef buffer = self.lastPixelBuffer;
  if (buffer == NULL) return;
  int64_t now = (int64_t)(CACurrentMediaTime() * 1e9);
  if (now - self.lastFrameHostNs < (int64_t)(self.periodNs * kRepeatAfterPeriods)) return;
  self.lastFrameHostNs = now;
  [self deliverPixelBuffer:buffer timeStampNs:now];
}

- (void)deliverPixelBuffer:(CVPixelBufferRef)pixelBuffer timeStampNs:(int64_t)timeStampNs {
  id<RTCVideoFrameBuffer> rtcBuffer = [[RTCCVPixelBuffer alloc] initWithPixelBuffer:pixelBuffer];
  RTCVideoFrame *frame = [[RTCVideoFrame alloc] initWithBuffer:rtcBuffer
                                                      rotation:RTCVideoRotation_0
                                                   timeStampNs:timeStampNs];
  [self.delegate capturer:self.capturer didCaptureVideoFrame:frame];
}

#if __has_include(<ScreenCaptureKit/ScreenCaptureKit.h>)
- (SCDisplay *)selectDisplayFromContent:(SCShareableContent *)content
                               sourceId:(NSString *)sourceId API_AVAILABLE(macos(12.3)) {
  if (content.displays.count == 0) {
    return nil;
  }

  if (sourceId != nil && sourceId.length > 0) {
    for (SCDisplay *display in content.displays) {
      if ([[NSString stringWithFormat:@"%u", display.displayID] isEqualToString:sourceId]) {
        return display;
      }
    }
  }

  CGDirectDisplayID mainDisplay = CGMainDisplayID();
  for (SCDisplay *display in content.displays) {
    if (display.displayID == mainDisplay) {
      return display;
    }
  }

  return content.displays.firstObject;
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error API_AVAILABLE(macos(12.3)) {
  NSLog(@"ScreenCaptureKit: stream stopped: %@", error);
}

- (void)stream:(SCStream *)stream
didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
        ofType:(SCStreamOutputType)type API_AVAILABLE(macos(12.3)) {
  if (type != SCStreamOutputTypeScreen) {
    return;
  }

  // Статус кадра: ScreenCaptureKit шлёт сэмплы и без картинки (Idle, Blank),
  // у них нет pixel buffer либо он старый. Берём только Complete.
  CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
  if (attachments != NULL && CFArrayGetCount(attachments) > 0) {
    CFDictionaryRef attachment = (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
    CFNumberRef statusRef =
        (CFNumberRef)CFDictionaryGetValue(attachment, (__bridge CFStringRef)SCStreamFrameInfoStatus);
    if (statusRef != NULL) {
      int status = 0;
      CFNumberGetValue(statusRef, kCFNumberIntType, &status);
      if (status != SCFrameStatusComplete) {
        return;
      }
    }
  }

  CVPixelBufferRef pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
  if (pixelBuffer == nil) {
    return;
  }

  // Метка времени — часы хоста, те же, что у webrtc: по ней он считает
  // каденцию и задержку. PTS сэмпла и так в этом домене, но держим один
  // источник для повторов и для настоящих кадров.
  int64_t timeStampNs = (int64_t)(CACurrentMediaTime() * 1e9);
  self.lastFrameHostNs = timeStampNs;

  CVPixelBufferRef previous = self.lastPixelBuffer;
  self.lastPixelBuffer = CVPixelBufferRetain(pixelBuffer);
  if (previous != NULL) CVPixelBufferRelease(previous);

  [self deliverPixelBuffer:pixelBuffer timeStampNs:timeStampNs];
}
#endif

@end
