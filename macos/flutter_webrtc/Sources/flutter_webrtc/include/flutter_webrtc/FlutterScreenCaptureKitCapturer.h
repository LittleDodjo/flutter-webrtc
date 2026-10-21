#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <WebRTC/WebRTC.h>

NS_ASSUME_NONNULL_BEGIN

@interface FlutterScreenCaptureKitCapturer : NSObject

- (instancetype)initWithDelegate:(id<RTCVideoCapturerDelegate>)delegate;

- (void)startCaptureWithFPS:(NSInteger)fps
                   sourceId:(NSString* _Nullable)sourceId
                  onStarted:(void (^ _Nonnull)(NSError * _Nullable error))onStarted;

// targetWidth/targetHeight — габарит, в который вписывается экран с
// сохранением пропорций (масштабирует ScreenCaptureKit на видеокарте). Ноль —
// физическое разрешение экрана в пикселях.
- (void)startCaptureWithFPS:(NSInteger)fps
                targetWidth:(NSInteger)targetWidth
               targetHeight:(NSInteger)targetHeight
                   sourceId:(NSString* _Nullable)sourceId
                  onStarted:(void (^ _Nonnull)(NSError * _Nullable error))onStarted;

- (void)stopCaptureWithCompletion:(void (^ _Nonnull)(void))completion;

@end

NS_ASSUME_NONNULL_END
