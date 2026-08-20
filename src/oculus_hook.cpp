// Optional in-process capture for VaM's native Oculus/Virtual Desktop path.
//
// Virtual Desktop's Oculus compatibility layer presents the game through the
// standard LibOVR C API.  OpenXR intentionally has no "read another app's
// compositor image" API, so this small opt-in hook observes the application's
// own ovr_SubmitFrame call and exports copies of its two submitted eye textures
// through keyed D3D11 shared resources.  The out-of-process VR180Mirror app
// performs all equirectangular reprojection and encoding as before.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "oculus_capture_shared.h"

using Microsoft::WRL::ComPtr;
namespace capture = vr180::oculus_capture;

namespace {

using OvrResult = std::int32_t;
using SubmitFrameFn = OvrResult(__cdecl*)(void*, std::int64_t, const void*,
                                          const void* const*, std::uint32_t);
using GetCurrentIndexFn = OvrResult(__cdecl*)(void*, void*, int*);
using GetBufferDxFn = OvrResult(__cdecl*)(void*, void*, int, REFIID, void**);
using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);

// These are the stable leading fields of the public LibOVR 1.x EyeFov layer.
// RenderPose immediately follows Fov for both EyeFov and EyeFovDepth.  It is
// deliberately copied with the same frameSerial as the texture pair so the
// out-of-process stabilizer never mixes a current pose with an older image.
struct Vector2i { int x; int y; };
struct Sizei { int w; int h; };
struct Recti { Vector2i pos; Sizei size; };
struct FovPort { float upTan; float downTan; float leftTan; float rightTan; };
struct Quatf { float x; float y; float z; float w; };
struct Vector3f { float x; float y; float z; };
struct Posef { Quatf orientation; Vector3f position; };
struct LayerHeader { int type; unsigned int flags; };
struct LayerEyeFovPrefix {
  LayerHeader header;
  void* colorTexture[2];
  Recti viewport[2];
  FovPort fov[2];
};
struct LayerEyeFovFrame {
  LayerEyeFovPrefix prefix;
  Posef renderPose[2];
  double sensorSampleTime;
};
static_assert(sizeof(Recti) == 16U);
static_assert(sizeof(FovPort) == 16U);
static_assert(offsetof(LayerEyeFovPrefix, colorTexture) == 8U);
static_assert(offsetof(LayerEyeFovPrefix, viewport) == 24U);
static_assert(offsetof(LayerEyeFovPrefix, fov) == 56U);
static_assert(offsetof(LayerEyeFovFrame, renderPose) == 88U);
static_assert(offsetof(LayerEyeFovFrame, sensorSampleTime) == 144U);

constexpr int kLayerTypeEyeFov = 1;
constexpr int kLayerTypeEyeFovDepth = 2;

GetProcAddressFn g_real_get_proc = ::GetProcAddress;
std::atomic<SubmitFrameFn> g_real_submit_frame{nullptr};
std::atomic<SubmitFrameFn> g_real_submit_frame2{nullptr};
std::atomic<SubmitFrameFn> g_real_end_frame{nullptr};
std::atomic<HMODULE> g_ovr_module{nullptr};
std::atomic<GetCurrentIndexFn> g_get_current_index{nullptr};
std::atomic<GetBufferDxFn> g_get_buffer_dx{nullptr};
std::atomic<bool> g_direct_submit_hook{false};

HANDLE g_mapping = nullptr;
capture::SharedState* g_shared = nullptr;
std::uint64_t g_epoch = 0U;

struct SharedEyeTexture {
  ComPtr<ID3D11Texture2D> texture;
  ComPtr<IDXGIKeyedMutex> mutex;
  HANDLE handle = nullptr;
  D3D11_TEXTURE2D_DESC desc{};
  Recti source_rect{};
};
SharedEyeTexture g_eye_textures[2];

bool IsEyeLayer(const LayerHeader* header) {
  return header != nullptr &&
         (header->type == kLayerTypeEyeFov || header->type == kLayerTypeEyeFovDepth);
}

bool EnsureSharedMapping() {
  if (g_shared != nullptr) return true;
  g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                 0, sizeof(capture::SharedState),
                                 capture::kMappingName);
  if (g_mapping == nullptr) return false;
  g_shared = static_cast<capture::SharedState*>(
      MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*g_shared)));
  if (g_shared == nullptr) {
    CloseHandle(g_mapping);
    g_mapping = nullptr;
    return false;
  }
  std::memset(g_shared, 0, sizeof(*g_shared));
  g_shared->magic = capture::kMagic;
  g_shared->version = capture::kVersion;
  g_shared->bytes = sizeof(*g_shared);
  g_shared->producerPid = GetCurrentProcessId();
  return true;
}

bool SameSource(const SharedEyeTexture& out, const D3D11_TEXTURE2D_DESC& source,
                const Recti& rect) {
  return out.texture != nullptr && out.desc.Width == static_cast<UINT>(rect.size.w) &&
         out.desc.Height == static_cast<UINT>(rect.size.h) && out.desc.Format == source.Format &&
         out.source_rect.pos.x == rect.pos.x && out.source_rect.pos.y == rect.pos.y &&
         out.source_rect.size.w == rect.size.w && out.source_rect.size.h == rect.size.h;
}

bool CreateSharedEye(const int eye, ID3D11Texture2D* source, const Recti& rect) {
  if (source == nullptr || rect.size.w <= 0 || rect.size.h <= 0) return false;
  D3D11_TEXTURE2D_DESC source_desc{};
  source->GetDesc(&source_desc);
  if (source_desc.SampleDesc.Count != 1U || rect.pos.x < 0 || rect.pos.y < 0 ||
      static_cast<std::uint64_t>(rect.pos.x + rect.size.w) > source_desc.Width ||
      static_cast<std::uint64_t>(rect.pos.y + rect.size.h) > source_desc.Height) {
    return false;
  }
  SharedEyeTexture& out = g_eye_textures[eye];
  if (SameSource(out, source_desc, rect)) return true;

  ComPtr<ID3D11Device> device;
  source->GetDevice(&device);
  if (!device) return false;
  D3D11_TEXTURE2D_DESC target{};
  target.Width = static_cast<UINT>(rect.size.w);
  target.Height = static_cast<UINT>(rect.size.h);
  target.MipLevels = 1U;
  target.ArraySize = 1U;
  target.Format = source_desc.Format;
  target.SampleDesc.Count = 1U;
  target.Usage = D3D11_USAGE_DEFAULT;
  target.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  target.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                     D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

  SharedEyeTexture replacement{};
  if (FAILED(device->CreateTexture2D(&target, nullptr, &replacement.texture))) return false;
  if (FAILED(replacement.texture.As(&replacement.mutex))) return false;
  ComPtr<IDXGIResource1> resource;
  if (FAILED(replacement.texture.As(&resource)) ||
      FAILED(resource->CreateSharedHandle(nullptr,
          DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
          nullptr, &replacement.handle)) || replacement.handle == nullptr) {
    return false;
  }
  replacement.desc = target;
  replacement.source_rect = rect;
  if (out.handle != nullptr) CloseHandle(out.handle);
  out = std::move(replacement);
  ++g_epoch;
  return true;
}

void PublishState(const LayerEyeFovFrame& layer) {
  if (!EnsureSharedMapping()) return;
  g_shared->producerPid = GetCurrentProcessId();
  g_shared->epoch = g_epoch;
  for (int eye = 0; eye < 2; ++eye) {
    const SharedEyeTexture& src = g_eye_textures[eye];
    capture::Eye& dst = eye == 0 ? g_shared->left : g_shared->right;
    dst.sharedHandle = reinterpret_cast<std::uint64_t>(src.handle);
    dst.width = src.desc.Width;
    dst.height = src.desc.Height;
    dst.format = static_cast<std::uint32_t>(src.desc.Format);
    dst.sampleCount = src.desc.SampleDesc.Count;
    dst.upTan = layer.prefix.fov[eye].upTan;
    dst.downTan = layer.prefix.fov[eye].downTan;
    dst.leftTan = layer.prefix.fov[eye].leftTan;
    dst.rightTan = layer.prefix.fov[eye].rightTan;
  }
  // The two eye orientations should be equivalent apart from position.  Use
  // the left-eye value, which is the exact pose Unity rendered into texture 0.
  const Quatf& q = layer.renderPose[0].orientation;
  const float norm2 = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
  capture::SourcePose& pose = g_shared->pose;
  pose.x = q.x; pose.y = q.y; pose.z = q.z; pose.w = q.w;
  pose.sensorSampleTime = layer.sensorSampleTime;
  pose.valid = norm2 > 0.25F && norm2 < 4.0F ? 1U : 0U;
  MemoryBarrier();
  ++g_shared->frameSerial;
  g_shared->hookState = 4U;
}

void CopySubmittedEyes(void* session, const LayerEyeFovFrame& layer) {
  const LayerEyeFovPrefix& prefix = layer.prefix;
  const auto get_index = g_get_current_index.load(std::memory_order_acquire);
  const auto get_buffer = g_get_buffer_dx.load(std::memory_order_acquire);
  if (get_index == nullptr || get_buffer == nullptr || session == nullptr) {
    if (g_shared != nullptr) g_shared->hookState = 30U;
    return;
  }
  for (int eye = 0; eye < 2; ++eye) {
    int index = 0;
    if (get_index(session, prefix.colorTexture[eye], &index) < 0) {
      if (g_shared != nullptr) g_shared->hookState = 31U + static_cast<std::uint32_t>(eye);
      return;
    }
    ComPtr<ID3D11Texture2D> source;
    if (get_buffer(session, prefix.colorTexture[eye], index, IID_PPV_ARGS(&source)) < 0 || !source) {
      if (g_shared != nullptr) g_shared->hookState = 33U + static_cast<std::uint32_t>(eye);
      return;
    }
    if (!CreateSharedEye(eye, source.Get(), prefix.viewport[eye])) {
      if (g_shared != nullptr) g_shared->hookState = 35U + static_cast<std::uint32_t>(eye);
      return;
    }
  }
  ComPtr<ID3D11Device> device;
  g_eye_textures[0].texture->GetDevice(&device);
  ComPtr<ID3D11DeviceContext> context;
  if (!device) { if (g_shared != nullptr) g_shared->hookState = 37U; return; }
  device->GetImmediateContext(&context);
  if (!context) { if (g_shared != nullptr) g_shared->hookState = 38U; return; }
  // The producer owns key 0; skip rather than ever stalling the game thread.
  if (g_eye_textures[0].mutex->AcquireSync(0U, 0U) != S_OK) {
    if (g_shared != nullptr) g_shared->hookState = 39U;
    return;
  }
  if (g_eye_textures[1].mutex->AcquireSync(0U, 0U) != S_OK) {
    g_eye_textures[0].mutex->ReleaseSync(0U);
    if (g_shared != nullptr) g_shared->hookState = 40U;
    return;
  }
  bool copied_both_eyes = true;
  for (int eye = 0; eye < 2; ++eye) {
    int index = 0;
    if (get_index(session, prefix.colorTexture[eye], &index) < 0) {
      copied_both_eyes = false;
      if (g_shared != nullptr) g_shared->hookState = 41U + static_cast<std::uint32_t>(eye);
      break;
    }
    ComPtr<ID3D11Texture2D> source;
    if (get_buffer(session, prefix.colorTexture[eye], index, IID_PPV_ARGS(&source)) < 0 || !source) {
      copied_both_eyes = false;
      if (g_shared != nullptr) g_shared->hookState = 43U + static_cast<std::uint32_t>(eye);
      break;
    }
    const Recti& r = prefix.viewport[eye];
    D3D11_BOX box{static_cast<UINT>(r.pos.x), static_cast<UINT>(r.pos.y), 0U,
                  static_cast<UINT>(r.pos.x + r.size.w), static_cast<UINT>(r.pos.y + r.size.h), 1U};
    context->CopySubresourceRegion(g_eye_textures[eye].texture.Get(), 0U, 0U, 0U, 0U,
                                   source.Get(), 0U, &box);
  }
  context->Flush();
  g_eye_textures[0].mutex->ReleaseSync(1U);
  g_eye_textures[1].mutex->ReleaseSync(1U);
  if (copied_both_eyes) PublishState(layer);
}

OvrResult CaptureAndForward(SubmitFrameFn real, void* session, std::int64_t frame_index,
                            const void* view_scale, const void* const* layers,
                            std::uint32_t layer_count) {
  __try {
    if (g_shared != nullptr) g_shared->hookState = 3U;
    if (layers != nullptr) {
      for (std::uint32_t i = 0; i < layer_count; ++i) {
        const auto* header = static_cast<const LayerHeader*>(layers[i]);
        if (IsEyeLayer(header)) {
          CopySubmittedEyes(session, *reinterpret_cast<const LayerEyeFovFrame*>(header));
          break;
        }
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // A failed capture must never take down the game; delegate the frame.
  }
  return real != nullptr ? real(session, frame_index, view_scale, layers, layer_count) : -1000;
}

OvrResult __cdecl HookedSubmitFrame(void* session, std::int64_t frame_index,
                                    const void* view_scale, const void* const* layers,
                                    std::uint32_t layer_count) {
  return CaptureAndForward(g_real_submit_frame.load(std::memory_order_acquire), session,
                           frame_index, view_scale, layers, layer_count);
}

OvrResult __cdecl HookedSubmitFrame2(void* session, std::int64_t frame_index,
                                     const void* view_scale, const void* const* layers,
                                     std::uint32_t layer_count) {
  return CaptureAndForward(g_real_submit_frame2.load(std::memory_order_acquire), session,
                           frame_index, view_scale, layers, layer_count);
}

OvrResult __cdecl HookedEndFrame(void* session, std::int64_t frame_index,
                                 const void* view_scale, const void* const* layers,
                                 std::uint32_t layer_count) {
  return CaptureAndForward(g_real_end_frame.load(std::memory_order_acquire), session,
                           frame_index, view_scale, layers, layer_count);
}

// OVRPlugin normally resolves LibOVR through GetProcAddress, which the IAT
// hook below observes.  Some Unity startup paths resolve it before an injected
// DLL gets CPU time.  Virtual Desktop's current x64 SubmitFrame/SubmitFrame2
// export shares this simple, non-relative 16-byte prologue; in that narrow
// case install a trampoline at the export itself.  Refuse any unknown prologue
// so an updated runtime remains a harmless "waiting for frame" rather than a
// game crash.
constexpr std::size_t kDirectPatchBytes = 16U;

bool WriteAbsoluteJump(void* from, const void* to, const std::size_t bytes) {
  if (from == nullptr || to == nullptr || bytes < 12U) return false;
  auto* code = static_cast<std::uint8_t*>(from);
  code[0] = 0x48U;  // mov rax, imm64
  code[1] = 0xB8U;
  const auto destination = reinterpret_cast<std::uintptr_t>(to);
  std::memcpy(code + 2U, &destination, sizeof(destination));
  code[10] = 0xFFU;  // jmp rax
  code[11] = 0xE0U;
  std::memset(code + 12U, 0x90, bytes - 12U);
  return true;
}

SubmitFrameFn InstallKnownDirectSubmitHook(void* target) {
  static constexpr std::uint8_t kKnownPrologue[kDirectPatchBytes] = {
      0x48U, 0x89U, 0x5CU, 0x24U, 0x10U, 0x56U, 0x48U, 0x83U,
      0xECU, 0x30U, 0x48U, 0x8BU, 0xDAU, 0x48U, 0x8BU, 0xF1U,
  };
  if (target == nullptr || std::memcmp(target, kKnownPrologue, kDirectPatchBytes) != 0) {
    return nullptr;
  }
  auto* trampoline = static_cast<std::uint8_t*>(VirtualAlloc(
      nullptr, kDirectPatchBytes + 12U, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (trampoline == nullptr) return nullptr;
  std::memcpy(trampoline, target, kDirectPatchBytes);
  if (!WriteAbsoluteJump(trampoline + kDirectPatchBytes,
                         static_cast<std::uint8_t*>(target) + kDirectPatchBytes, 12U)) {
    VirtualFree(trampoline, 0U, MEM_RELEASE);
    return nullptr;
  }
  DWORD old_protect = 0U;
  if (!VirtualProtect(target, kDirectPatchBytes, PAGE_EXECUTE_READWRITE, &old_protect) ||
      !WriteAbsoluteJump(target, reinterpret_cast<void*>(&HookedSubmitFrame2), kDirectPatchBytes)) {
    if (old_protect != 0U) {
      DWORD ignored = 0U;
      VirtualProtect(target, kDirectPatchBytes, old_protect, &ignored);
    }
    VirtualFree(trampoline, 0U, MEM_RELEASE);
    return nullptr;
  }
  DWORD ignored = 0U;
  VirtualProtect(target, kDirectPatchBytes, old_protect, &ignored);
  FlushInstructionCache(GetCurrentProcess(), target, kDirectPatchBytes);
  FlushInstructionCache(GetCurrentProcess(), trampoline, kDirectPatchBytes + 12U);
  return reinterpret_cast<SubmitFrameFn>(trampoline);
}

bool TryInstallDirectVirtualDesktopSubmitHook() {
  if (g_direct_submit_hook.load(std::memory_order_acquire)) return true;
  HMODULE runtime = GetModuleHandleW(L"VirtualDesktop.LibOVRRT64_1.dll");
  if (runtime == nullptr) return false;
  const auto submit = reinterpret_cast<void*>(GetProcAddress(runtime, "ovr_SubmitFrame2"));
  const auto trampoline = InstallKnownDirectSubmitHook(submit);
  if (trampoline == nullptr) return false;
  g_real_submit_frame.store(trampoline, std::memory_order_release);
  g_real_submit_frame2.store(trampoline, std::memory_order_release);
  g_ovr_module.store(runtime, std::memory_order_release);
  g_get_current_index.store(reinterpret_cast<GetCurrentIndexFn>(
      GetProcAddress(runtime, "ovr_GetTextureSwapChainCurrentIndex")), std::memory_order_release);
  g_get_buffer_dx.store(reinterpret_cast<GetBufferDxFn>(
      GetProcAddress(runtime, "ovr_GetTextureSwapChainBufferDX")), std::memory_order_release);
  g_direct_submit_hook.store(true, std::memory_order_release);
  if (g_shared != nullptr) g_shared->hookState = 2U;
  return true;
}

FARPROC WINAPI HookedGetProcAddress(HMODULE module, LPCSTR name) {
  const FARPROC resolved = g_real_get_proc(module, name);
  if (name == nullptr || IS_INTRESOURCE(name) || resolved == nullptr ||
      (std::strcmp(name, "ovr_SubmitFrame") != 0 &&
       std::strcmp(name, "ovr_SubmitFrame2") != 0 &&
       std::strcmp(name, "ovr_EndFrame") != 0)) {
    return resolved;
  }
  g_ovr_module.store(module, std::memory_order_release);
  FARPROC replacement = nullptr;
  if (std::strcmp(name, "ovr_SubmitFrame") == 0) {
    if (!g_direct_submit_hook.load(std::memory_order_acquire)) {
      g_real_submit_frame.store(reinterpret_cast<SubmitFrameFn>(resolved), std::memory_order_release);
    }
    replacement = reinterpret_cast<FARPROC>(&HookedSubmitFrame);
  } else if (std::strcmp(name, "ovr_SubmitFrame2") == 0) {
    if (!g_direct_submit_hook.load(std::memory_order_acquire)) {
      g_real_submit_frame2.store(reinterpret_cast<SubmitFrameFn>(resolved), std::memory_order_release);
    }
    replacement = reinterpret_cast<FARPROC>(&HookedSubmitFrame2);
  } else {
    g_real_end_frame.store(reinterpret_cast<SubmitFrameFn>(resolved), std::memory_order_release);
    replacement = reinterpret_cast<FARPROC>(&HookedEndFrame);
  }
  g_get_current_index.store(reinterpret_cast<GetCurrentIndexFn>(
      g_real_get_proc(module, "ovr_GetTextureSwapChainCurrentIndex")), std::memory_order_release);
  g_get_buffer_dx.store(reinterpret_cast<GetBufferDxFn>(
      g_real_get_proc(module, "ovr_GetTextureSwapChainBufferDX")), std::memory_order_release);
  if (g_shared != nullptr) g_shared->hookState = 2U;
  // PC LibOVR exposes three historical names for this frame/layer ABI.
  // Different OVRPlugin revisions resolve one or more of them dynamically;
  // capture them equivalently without affecting the submitted call.
  return replacement;
}

bool PatchOvrPluginGetProcAddress(HMODULE module) {
  auto* base = reinterpret_cast<std::uint8_t*>(module);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
  const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (dir.VirtualAddress == 0U) return false;
  auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
  for (; desc->Name != 0U; ++desc) {
    auto* first = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
    auto* original = reinterpret_cast<IMAGE_THUNK_DATA*>(base +
        (desc->OriginalFirstThunk != 0U ? desc->OriginalFirstThunk : desc->FirstThunk));
    for (; original->u1.AddressOfData != 0U; ++original, ++first) {
      if (IMAGE_SNAP_BY_ORDINAL(original->u1.Ordinal)) continue;
      auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + original->u1.AddressOfData);
      if (std::strcmp(reinterpret_cast<const char*>(import->Name), "GetProcAddress") != 0) continue;
      void* replacement = reinterpret_cast<void*>(&HookedGetProcAddress);
      DWORD old_protect = 0U;
      if (!VirtualProtect(&first->u1.Function, sizeof(first->u1.Function), PAGE_READWRITE, &old_protect)) return false;
      first->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
      DWORD ignored = 0U;
      VirtualProtect(&first->u1.Function, sizeof(first->u1.Function), old_protect, &ignored);
      FlushInstructionCache(GetCurrentProcess(), &first->u1.Function, sizeof(first->u1.Function));
      return true;
    }
  }
  return false;
}

DWORD WINAPI HookWorker(void*) {
  // Arm before VaM reaches OVRPlugin's runtime resolver.  The launcher starts
  // this hook while Unity is still loading, but remain patient for late loads.
  bool iat_patched = false;
  for (int attempt = 0; attempt < 600; ++attempt) {
    HMODULE plugin = GetModuleHandleW(L"OVRPlugin.dll");
    if (!iat_patched && plugin != nullptr && PatchOvrPluginGetProcAddress(plugin)) {
      // Publish an armed producer immediately.  VR180Mirror can distinguish a
      // successfully injected but not-yet-rendering game from a failed inject.
      if (EnsureSharedMapping()) g_shared->hookState = 1U;
      iat_patched = true;
    }
    // If Unity resolved SubmitFrame before the IAT patch, detour the known VD
    // runtime export instead.  Do this only while no dynamic resolution has
    // been observed, so the two interception paths cannot chain into each
    // other.
    if (iat_patched && g_shared != nullptr && g_shared->hookState == 1U &&
        TryInstallDirectVirtualDesktopSubmitHook()) {
      return 0U;
    }
    Sleep(50U);
  }
  return 0U;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(module);
    HANDLE worker = CreateThread(nullptr, 0U, HookWorker, nullptr, 0U, nullptr);
    if (worker != nullptr) CloseHandle(worker);
  }
  return TRUE;
}
