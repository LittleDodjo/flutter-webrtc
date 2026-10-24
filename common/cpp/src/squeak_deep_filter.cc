#include "squeak_deep_filter.h"

#include <cmath>
#include <cstdio>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace flutter_webrtc_plugin {
namespace {

// Без предела подавления: с пределом 30 дБ клавиатура в паузах оставалась на
// −62 dBFS вместо −71, а речь не менялась.
constexpr float kAttenLimDb = 100.0f;

// Пост-фильтр DFN на нашем прогоне ничего не дал.
constexpr float kPostFilterBeta = 0.0f;

constexpr float kInt16Scale = 32768.0f;

#if defined(_WIN32)
constexpr const wchar_t* kLibraryName = L"squeak_df.dll";

// Рядом с нашим же модулем: Flutter кладёт DLL плагинов к exe, но искать по
// своему адресу надёжнее, чем по текущему каталогу.
void* OpenLibrary() {
  HMODULE self = nullptr;
  std::wstring path = kLibraryName;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&OpenLibrary), &self)) {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(self, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
      std::wstring dir(buf, n);
      const size_t slash = dir.find_last_of(L"\\/");
      if (slash != std::wstring::npos) path = dir.substr(0, slash + 1) + kLibraryName;
    }
  }
  return LoadLibraryW(path.c_str());
}

void* Symbol(void* lib, const char* name) {
  return reinterpret_cast<void*>(
      GetProcAddress(static_cast<HMODULE>(lib), name));
}
#else
constexpr const char* kLibraryName = "libsqueak_df.so";

// lib/ бандла, где лежит и сам плагин.
void* OpenLibrary() {
  std::string path = kLibraryName;
  Dl_info info;
  if (dladdr(reinterpret_cast<void*>(&OpenLibrary), &info) != 0 &&
      info.dli_fname != nullptr) {
    std::string self = info.dli_fname;
    const size_t slash = self.find_last_of('/');
    if (slash != std::string::npos) path = self.substr(0, slash + 1) + kLibraryName;
  }
  void* lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  return lib != nullptr ? lib : dlopen(kLibraryName, RTLD_NOW | RTLD_LOCAL);
}

void* Symbol(void* lib, const char* name) { return dlsym(lib, name); }
#endif

}  // namespace

SqueakDeepFilter::~SqueakDeepFilter() {
  if (loader_.joinable()) loader_.join();
  if (state_ != nullptr && free_ != nullptr) free_(state_);
  // Библиотеку не выгружаем: она живёт до конца процесса.
}

void SqueakDeepFilter::Prepare() {
  std::call_once(once_, [this] {
    status_.store(Status::kLoading);
    loader_ = std::thread([this] { Load(); });
  });
}

void SqueakDeepFilter::Load() {
  void* lib = OpenLibrary();
  if (lib == nullptr) {
    std::fprintf(stderr, "[squeak] dfn3: библиотеки нет, остаётся RNNoise\n");
    status_.store(Status::kMissing);
    return;
  }
  auto create = reinterpret_cast<CreateFn>(Symbol(lib, "sqdf_create"));
  auto frame_length =
      reinterpret_cast<FrameLengthFn>(Symbol(lib, "sqdf_frame_length"));
  process_ = reinterpret_cast<ProcessFn>(Symbol(lib, "sqdf_process"));
  free_ = reinterpret_cast<FreeFn>(Symbol(lib, "sqdf_free"));
  if (create == nullptr || frame_length == nullptr || process_ == nullptr ||
      free_ == nullptr) {
    std::fprintf(stderr, "[squeak] dfn3: в библиотеке нет нужных функций\n");
    status_.store(Status::kFailed);
    return;
  }
  state_ = create(kAttenLimDb, kPostFilterBeta);
  if (state_ == nullptr || frame_length(state_) != kFrameSize) {
    std::fprintf(stderr, "[squeak] dfn3: модель не поднялась\n");
    status_.store(Status::kFailed);
    return;
  }
  // release: state_ и указатели видны аудиопотоку, прочитавшему kReady.
  status_.store(Status::kReady, std::memory_order_release);
}

bool SqueakDeepFilter::Process(float* buffer, float* lsnr) {
  if (status_.load(std::memory_order_acquire) != Status::kReady) return false;
  for (int i = 0; i < kFrameSize; ++i) in_[i] = buffer[i] / kInt16Scale;
  const float snr = process_(state_, in_, out_);
  if (std::isnan(snr)) {
    std::fprintf(stderr, "[squeak] dfn3: сбой модели, остаётся RNNoise\n");
    status_.store(Status::kFailed);
    return false;
  }
  for (int i = 0; i < kFrameSize; ++i) buffer[i] = out_[i] * kInt16Scale;
  *lsnr = snr;
  return true;
}

}  // namespace flutter_webrtc_plugin
