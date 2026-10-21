#include "flutter_video_renderer.h"

#include <algorithm>

#if defined(_WINDOWS)
#include <dxgi.h>

#include "libwebrtc.h"
#endif

namespace flutter_webrtc_plugin {

FlutterVideoRenderer::~FlutterVideoRenderer() {}

void FlutterVideoRenderer::initialize(
    TextureRegistrar* registrar,
    BinaryMessenger* messenger,
    TaskRunner* task_runner,
    std::unique_ptr<flutter::TextureVariant> texture,
    int64_t trxture_id) {
  registrar_ = registrar;
  texture_ = std::move(texture);
  texture_id_ = trxture_id;
  std::string channel_name =
      "FlutterWebRTC/Texture" + std::to_string(texture_id_);
  event_channel_ = EventChannelProxy::Create(messenger, task_runner, channel_name);
}

const FlutterDesktopPixelBuffer* FlutterVideoRenderer::CopyPixelBuffer(
    size_t width,
    size_t height) const {
  mutex_.lock();
  if (pixel_buffer_.get() && frame_.get()) {
    // Движок просит текстуру под её фактический размер на экране и после
    // вызова перечитывает реальный размер из pixel_buffer_. Конвертировать
    // кадр целиком незачем: поток 1440p в плитке 320x180 — это полноразмерный
    // ScaleFrom на CPU и 14 МБ заливки на каждый нарисованный кадр.
    int target_width = frame_->width();
    int target_height = frame_->height();
    if (width > 0 && height > 0 && target_width > 0 && target_height > 0) {
      // Пропорции держим сами и никогда не увеличиваем: виджет масштабирует
      // текстуру исходя из videoWidth/videoHeight, и непропорциональный ужим
      // дал бы искажение картинки.
      const double scale = (std::min)(
          {1.0, static_cast<double>(width) / target_width,
           static_cast<double>(height) / target_height});
      if (scale < 1.0) {
        target_width = (std::max)(2, static_cast<int>(target_width * scale) & ~1);
        target_height =
            (std::max)(2, static_cast<int>(target_height * scale) & ~1);
      }
    }

    if (pixel_buffer_->width != static_cast<size_t>(target_width) ||
        pixel_buffer_->height != static_cast<size_t>(target_height)) {
      size_t buffer_size =
          (size_t(target_width) * size_t(target_height)) * (32 >> 3);
      rgb_buffer_.reset(new uint8_t[buffer_size]);
      pixel_buffer_->width = target_width;
      pixel_buffer_->height = target_height;
    }

    frame_->ConvertToARGB(RTCVideoFrame::Type::kABGR, rgb_buffer_.get(), 0,
                          static_cast<int>(pixel_buffer_->width),
                          static_cast<int>(pixel_buffer_->height));

    pixel_buffer_->buffer = rgb_buffer_.get();
    mutex_.unlock();
    return pixel_buffer_.get();
  }
  mutex_.unlock();
  return nullptr;
}

#if defined(_WINDOWS)

bool FlutterVideoRenderer::GpuTexturesAvailable() {
  return LibWebRTC::GetD3D11Device() != nullptr;
}

bool FlutterVideoRenderer::EnsureGpuSurface(GpuSurface* surface, size_t width,
                                            size_t height) {
  if (surface->texture != nullptr && surface->width == width &&
      surface->height == height) {
    return true;
  }
  auto* device = static_cast<ID3D11Device*>(LibWebRTC::GetD3D11Device());
  if (device == nullptr) return false;

  surface->texture.Reset();
  surface->handle = nullptr;
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = static_cast<UINT>(width);
  desc.Height = static_cast<UINT>(height);
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  // Общий ресурс: движок открывает его на своём устройстве по хэндлу.
  desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
  if (FAILED(device->CreateTexture2D(&desc, nullptr, &surface->texture))) {
    return false;
  }
  Microsoft::WRL::ComPtr<IDXGIResource> resource;
  if (FAILED(surface->texture.As(&resource)) ||
      FAILED(resource->GetSharedHandle(&surface->handle))) {
    surface->texture.Reset();
    return false;
  }
  surface->width = width;
  surface->height = height;
  surface->descriptor = {};
  surface->descriptor.struct_size = sizeof(FlutterDesktopGpuSurfaceDescriptor);
  surface->descriptor.handle = surface->handle;
  surface->descriptor.width = width;
  surface->descriptor.height = height;
  surface->descriptor.visible_width = width;
  surface->descriptor.visible_height = height;
  surface->descriptor.format = kFlutterDesktopPixelFormatBGRA8888;
  surface->descriptor.release_callback = nullptr;
  surface->descriptor.release_context = nullptr;
  return true;
}

// Под mutex_. Кадр рисуется в задний буфер, после чего он становится передним.
bool FlutterVideoRenderer::RenderToGpuSurface(
    const scoped_refptr<RTCVideoFrame>& frame) {
  // Текстура в размер кадра, не плитки: движок масштабирует сам, а плитка
  // меняет размер при каждой перестановке сетки — пересоздавать текстуру под
  // неё было бы дороже, чем сэмплировать большую.
  int width = frame->width();
  int height = frame->height();
  const int rotation = frame->rotation();
  if (rotation == RTCVideoFrame::kVideoRotation_90 ||
      rotation == RTCVideoFrame::kVideoRotation_270) {
    // Поворот видеокарта в этом пути не делает — такие кадры (камера
    // телефона) идут через процессор.
    return false;
  }
  if (width <= 0 || height <= 0) return false;

  const int back = gpu_front_ < 0 ? 0 : 1 - gpu_front_;
  GpuSurface& surface = gpu_surfaces_[back];
  if (!EnsureGpuSurface(&surface, static_cast<size_t>(width),
                        static_cast<size_t>(height))) {
    return false;
  }
  if (!frame->RenderToD3D11Texture(surface.texture.Get(), 0, 0, width,
                                   height)) {
    return false;
  }
  // Движок читает текстуру на другом устройстве: команды должны уйти на
  // видеокарту до того, как он возьмёт хэндл.
  auto* device = static_cast<ID3D11Device*>(LibWebRTC::GetD3D11Device());
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  device->GetImmediateContext(&context);
  context->Flush();
  gpu_front_ = back;
  return true;
}

const FlutterDesktopGpuSurfaceDescriptor* FlutterVideoRenderer::ObtainGpuSurface(
    size_t /* width */, size_t /* height */) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (gpu_front_ < 0) return nullptr;
  return &gpu_surfaces_[gpu_front_].descriptor;
}

#endif  // _WINDOWS

void FlutterVideoRenderer::OnFrame(scoped_refptr<RTCVideoFrame> frame) {
  if (!first_frame_rendered) {
    EncodableMap params;
    params[EncodableValue("event")] = "didFirstFrameRendered";
    params[EncodableValue("id")] = EncodableValue(texture_id_);
    event_channel_->Success(EncodableValue(params));
    pixel_buffer_.reset(new FlutterDesktopPixelBuffer());
    pixel_buffer_->width = 0;
    pixel_buffer_->height = 0;
    first_frame_rendered = true;
  }
  if (rotation_ != frame->rotation()) {
    EncodableMap params;
    params[EncodableValue("event")] = "didTextureChangeRotation";
    params[EncodableValue("id")] = EncodableValue(texture_id_);
    params[EncodableValue("rotation")] =
        EncodableValue((int32_t)frame->rotation());
    event_channel_->Success(EncodableValue(params));
    rotation_ = frame->rotation();
  }
  if (last_frame_size_.width != frame->width() ||
      last_frame_size_.height != frame->height()) {
    EncodableMap params;
    params[EncodableValue("event")] = "didTextureChangeVideoSize";
    params[EncodableValue("id")] = EncodableValue(texture_id_);
    params[EncodableValue("width")] = EncodableValue((int32_t)frame->width());
    params[EncodableValue("height")] = EncodableValue((int32_t)frame->height());
    event_channel_->Success(EncodableValue(params));

    last_frame_size_ = {(size_t)frame->width(), (size_t)frame->height()};
  }
#if defined(_WINDOWS)
  if (gpu_path_) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!RenderToGpuSurface(frame)) return;
    }
    registrar_->MarkTextureFrameAvailable(texture_id_);
    return;
  }
#endif
  mutex_.lock();
  frame_ = frame;
  mutex_.unlock();
  registrar_->MarkTextureFrameAvailable(texture_id_);
}

void FlutterVideoRenderer::SetVideoTrack(scoped_refptr<RTCVideoTrack> track) {
  if (track_ != track) {
    if (track_)
      track_->RemoveRenderer(this);
    track_ = track;
    last_frame_size_ = {0, 0};
    first_frame_rendered = false;
    if (track_)
      track_->AddRenderer(this);
  }
}

bool FlutterVideoRenderer::CheckMediaStream(std::string mediaId) {
  if (0 == mediaId.size() || 0 == media_stream_id.size()) {
    return false;
  }
  return mediaId == media_stream_id;
}

bool FlutterVideoRenderer::CheckVideoTrack(std::string mediaId) {
  if (0 == mediaId.size() || !track_) {
    return false;
  }
  return mediaId == track_->id().std_string();
}

FlutterVideoRendererManager::FlutterVideoRendererManager(
    FlutterWebRTCBase* base)
    : base_(base) {}

void FlutterVideoRendererManager::CreateVideoRendererTexture(
    std::unique_ptr<MethodResultProxy> result) {
  auto texture = new RefCountedObject<FlutterVideoRenderer>();
  std::unique_ptr<flutter::TextureVariant> textureVariant;
#if defined(_WINDOWS)
  if (FlutterVideoRenderer::GpuTexturesAvailable()) {
    textureVariant = std::make_unique<flutter::TextureVariant>(
        flutter::GpuSurfaceTexture(
            kFlutterDesktopGpuSurfaceTypeDxgiSharedHandle,
            [texture](size_t width, size_t height)
                -> const FlutterDesktopGpuSurfaceDescriptor* {
              return texture->ObtainGpuSurface(width, height);
            }));
    texture->EnableGpuPath();
  }
#endif
  if (!textureVariant) {
    textureVariant =
        std::make_unique<flutter::TextureVariant>(flutter::PixelBufferTexture(
            [texture](size_t width,
                      size_t height) -> const FlutterDesktopPixelBuffer* {
              return texture->CopyPixelBuffer(width, height);
            }));
  }

  auto texture_id = base_->textures_->RegisterTexture(textureVariant.get());
  texture->initialize(base_->textures_, base_->messenger_, base_->task_runner_,
                      std::move(textureVariant), texture_id);
  renderers_[texture_id] = texture;
  EncodableMap params;
  params[EncodableValue("textureId")] = EncodableValue(texture_id);
  result->Success(EncodableValue(params));
}

void FlutterVideoRendererManager::VideoRendererSetSrcObject(
    int64_t texture_id,
    const std::string& stream_id,
    const std::string& owner_tag,
    const std::string& track_id) {
  scoped_refptr<RTCMediaStream> stream =
      base_->MediaStreamForId(stream_id, owner_tag);

  auto it = renderers_.find(texture_id);
  if (it != renderers_.end()) {
    FlutterVideoRenderer* renderer = it->second.get();
    if (stream.get()) {
      auto video_tracks = stream->video_tracks();
      if (video_tracks.size() > 0) {
        if (track_id == std::string()) {
          renderer->SetVideoTrack(video_tracks[0]);
        } else {
          for (auto track : video_tracks.std_vector()) {
            if (track->id().std_string() == track_id) {
              renderer->SetVideoTrack(track);
              break;
            }
          }
        }
        renderer->media_stream_id = stream_id;
      }
    } else {
      renderer->SetVideoTrack(nullptr);
    }
  }
}

void FlutterVideoRendererManager::VideoRendererDispose(
    int64_t texture_id,
    std::unique_ptr<MethodResultProxy> result) {
  auto it = renderers_.find(texture_id);
  if (it != renderers_.end()) {
    it->second->SetVideoTrack(nullptr);
#if defined(_WINDOWS)
    base_->textures_->UnregisterTexture(texture_id,
                                        [&, it] { renderers_.erase(it); });
#else
    base_->textures_->UnregisterTexture(texture_id);
    renderers_.erase(it);
#endif
    result->Success();
    return;
  }
  result->Error("VideoRendererDisposeFailed",
                "VideoRendererDispose() texture not found!");
}

}  // namespace flutter_webrtc_plugin
