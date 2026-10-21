#ifndef FLUTTER_WEBRTC_RTC_VIDEO_RENDERER_HXX
#define FLUTTER_WEBRTC_RTC_VIDEO_RENDERER_HXX

#include "flutter_common.h"
#include "flutter_webrtc_base.h"

#include "rtc_video_frame.h"
#include "rtc_video_renderer.h"

#include <mutex>

#if defined(_WINDOWS)
#include <d3d11.h>
#include <wrl/client.h>
#endif

namespace flutter_webrtc_plugin {

using namespace libwebrtc;

class FlutterVideoRenderer
    : public RTCVideoRenderer<scoped_refptr<RTCVideoFrame>>,
      public RefCountInterface {
 public:
  FlutterVideoRenderer() = default;
  ~FlutterVideoRenderer();

  void initialize(TextureRegistrar* registrar,
                  BinaryMessenger* messenger,
                  TaskRunner* task_runner,
                  std::unique_ptr<flutter::TextureVariant> texture,
                  int64_t texture_id);

  virtual const FlutterDesktopPixelBuffer* CopyPixelBuffer(size_t width,
                                                           size_t height) const;

#if defined(_WINDOWS)
  // Текстурный путь: кадр из видеопамяти рисуется видеокартой в общую
  // BGRA-текстуру, которую движок Flutter открывает по DXGI-хэндлу. Пиксели
  // через процессор не ходят вовсе.
  const FlutterDesktopGpuSurfaceDescriptor* ObtainGpuSurface(size_t width,
                                                             size_t height);
  void EnableGpuPath() { gpu_path_ = true; }
  static bool GpuTexturesAvailable();
#endif

  virtual void OnFrame(scoped_refptr<RTCVideoFrame> frame) override;

  void SetVideoTrack(scoped_refptr<RTCVideoTrack> track);

  int64_t texture_id() { return texture_id_; }

  bool CheckMediaStream(std::string mediaId);

  bool CheckVideoTrack(std::string mediaId);

  std::string media_stream_id;

 private:
  struct FrameSize {
    size_t width;
    size_t height;
  };
#if defined(_WINDOWS)
  // Два буфера: пока движок читает один, следующий кадр ложится в другой.
  struct GpuSurface {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    HANDLE handle = nullptr;
    size_t width = 0;
    size_t height = 0;
    FlutterDesktopGpuSurfaceDescriptor descriptor = {};
  };
  bool RenderToGpuSurface(const scoped_refptr<RTCVideoFrame>& frame);
  bool EnsureGpuSurface(GpuSurface* surface, size_t width, size_t height);

  GpuSurface gpu_surfaces_[2];
  int gpu_front_ = -1;
  bool gpu_path_ = false;
#endif
  FrameSize last_frame_size_ = {0, 0};
  bool first_frame_rendered = false;
  TextureRegistrar* registrar_ = nullptr;
  std::unique_ptr<EventChannelProxy> event_channel_;
  int64_t texture_id_ = -1;
  scoped_refptr<RTCVideoTrack> track_ = nullptr;
  scoped_refptr<RTCVideoFrame> frame_;
  std::unique_ptr<flutter::TextureVariant> texture_;
  std::shared_ptr<FlutterDesktopPixelBuffer> pixel_buffer_;
  mutable std::shared_ptr<uint8_t[]> rgb_buffer_;
  mutable std::mutex mutex_;
  RTCVideoFrame::VideoRotation rotation_ = RTCVideoFrame::kVideoRotation_0;
};

class FlutterVideoRendererManager {
 public:
  FlutterVideoRendererManager(FlutterWebRTCBase* base);

  void CreateVideoRendererTexture(std::unique_ptr<MethodResultProxy> result);

  void VideoRendererSetSrcObject(int64_t texture_id,
                                 const std::string& stream_id,
                                 const std::string& owner_tag,
                                 const std::string& track_id);

  void VideoRendererDispose(int64_t texture_id,
                            std::unique_ptr<MethodResultProxy> result);

 private:
  FlutterWebRTCBase* base_;
  std::map<int64_t, scoped_refptr<FlutterVideoRenderer>> renderers_;
};

}  // namespace flutter_webrtc_plugin

#endif  // !FLUTTER_WEBRTC_RTC_VIDEO_RENDERER_HXX
