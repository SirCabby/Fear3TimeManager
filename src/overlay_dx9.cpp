// D3D9 backend (only used when the game runs with -d3d9 in options.cfg). The
// game resolves Direct3DCreate9 through GetProcAddress; from the returned
// IDirect3D9 we hook CreateDevice (slot 16, in place: with the Steam overlay
// that object is the overlay's own wrapper), then EndScene (42) and Reset (16)
// on the device - on Windows in a vtable of the game's device's own (adopt.cpp:
// the overlay hooks what the device class's Reset points to, at every
// CreateDevice), under Wine in the class's vtable, as before. Ported from
// Fear2AwardUnlocker's overlay.cpp.

#include <windows.h>
#include <d3d9.h>

#include "overlay.h"

#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"
#include "adopt.h"
#include "crash.h"
#include "log.h"
#include "mem.h"

namespace f3tm::overlay::dx9 {
namespace {

using Create9Fn = IDirect3D9*(WINAPI*)(UINT);
using CreateDeviceFn = HRESULT(__stdcall*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                           D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using EndSceneFn = HRESULT(__stdcall*)(IDirect3DDevice9*);
using ResetFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

Create9Fn g_real_create9 = nullptr;
CreateDeviceFn g_orig_create_device = nullptr;
EndSceneFn g_orig_end_scene = nullptr;
ResetFn g_orig_reset = nullptr;
uintptr_t g_d3d_vt = 0;
uintptr_t g_device_vt = 0;  // the class hook's (Wine)

constexpr int kReset = 16, kEndScene = 42;
// IDirect3DDevice9's slots (IDirect3DDevice9Ex has 134); an adopted device's
// copy has as many as can be read, up to kDeviceCopy.
constexpr int kDeviceSlots = 119, kDeviceCopy = 192;
IDirect3DDevice9* g_device = nullptr;
HWND g_hwnd = nullptr;
bool g_renderer_ready = false;

bool ensure_renderer(IDirect3DDevice9* device) {
  if (g_renderer_ready) return true;
  if (!g_hwnd) {
    D3DDEVICE_CREATION_PARAMETERS cp{};
    if (SUCCEEDED(device->GetCreationParameters(&cp))) g_hwnd = cp.hFocusWindow;
  }
  if (!ensure_context(g_hwnd)) return false;
  if (!ImGui_ImplDX9_Init(device)) {
    logf("ERROR: dx9: ImGui DX9 backend init failed");
    return false;
  }
  g_renderer_ready = true;
  return true;
}

HRESULT __stdcall hk_end_scene(IDirect3DDevice9* device) {
  if (!adopt::calling_on() && claim(Backend::kDx9) && ensure_renderer(device) && wants_draw()) {
    // The game leaves render state wherever the last draw put it; ImGui's DX9
    // backend sets up what it needs but does not restore ours.
    IDirect3DStateBlock9* state = nullptr;
    device->CreateStateBlock(D3DSBT_ALL, &state);
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    draw_panel();
    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    if (state) {
      state->Apply();
      state->Release();
    }
  }
  const auto orig = reinterpret_cast<EndSceneFn>(
      adopt::original(device, kEndScene, reinterpret_cast<void*>(g_orig_end_scene)));
  adopt::CallingOn on;
  return orig(device);
}

HRESULT __stdcall hk_reset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp) {
  if (g_renderer_ready) ImGui_ImplDX9_InvalidateDeviceObjects();
  const auto orig = reinterpret_cast<ResetFn>(adopt::original(device, kReset, reinterpret_cast<void*>(g_orig_reset)));
  HRESULT hr;
  {
    adopt::CallingOn on;
    hr = orig(device, pp);
  }
  if (g_renderer_ready) ImGui_ImplDX9_CreateDeviceObjects();
  return hr;
}

HRESULT __stdcall hk_create_device(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND focus,
                                   DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
  HRESULT hr = g_orig_create_device(self, adapter, type, focus, flags, pp, out);
  if (FAILED(hr) || !out || !*out) return hr;
  IDirect3DDevice9* device = *out;
  HWND hwnd = focus ? focus : pp ? pp->hDeviceWindow : nullptr;
  auto vt = mem::read<uintptr_t>(reinterpret_cast<uintptr_t>(device));
  if (adopt::enabled()) {
    if (!adopt::game_window(hwnd)) {
      adopt::leave_alone("dx9: a device", "CreateDevice", hwnd);
      return hr;
    }
    static const adopt::Hook hooks[] = {{kEndScene, reinterpret_cast<void*>(&hk_end_scene)},
                                        {kReset, reinterpret_cast<void*>(&hk_reset)}};
    // What the hooks fall back on: the class's functions (its vtable is left alone).
    if (!g_orig_end_scene) {
      g_orig_end_scene = mem::read<EndSceneFn>(vt + kEndScene * sizeof(void*));
      g_orig_reset = mem::read<ResetFn>(vt + kReset * sizeof(void*));
    }
    const int copied = adopt::take(device, kDeviceSlots, kDeviceCopy, hooks, 2);
    char where[MAX_PATH + 32];
    describe_address(reinterpret_cast<void*>(vt), where, sizeof(where));
    if (!copied) {
      logf("ERROR: dx9: the game's device %p (hwnd=%p) could not be adopted (its vtable at %s) - no panel", device, hwnd,
           where);
      return hr;
    }
    char win[128];
    adopt::describe_window(hwnd, win, sizeof(win));
    logf("dx9: device %p (%s) adopted - a vtable of its own with EndScene/Reset (%d slots copied; the class's at %s)",
         device, win, copied, where);
  } else if (!g_orig_end_scene) {
    g_orig_end_scene = reinterpret_cast<EndSceneFn>(mem::hook_vtable(vt, kEndScene, reinterpret_cast<void*>(&hk_end_scene)));
    g_orig_reset = reinterpret_cast<ResetFn>(mem::hook_vtable(vt, kReset, reinterpret_cast<void*>(&hk_reset)));
    g_device_vt = vt;
    logf("dx9: device %p captured (hwnd=%p); hooked EndScene/Reset", device, hwnd);
  }
  g_device = device;
  if (hwnd) g_hwnd = hwnd;
  return hr;
}

IDirect3D9* WINAPI hk_create9(UINT sdk) {
  IDirect3D9* d3d = g_real_create9(sdk);
  if (d3d && !g_orig_create_device) {
    auto vt = mem::read<uintptr_t>(reinterpret_cast<uintptr_t>(d3d));
    g_orig_create_device = reinterpret_cast<CreateDeviceFn>(
        mem::hook_vtable(vt, 16, reinterpret_cast<void*>(&hk_create_device)));
    g_d3d_vt = vt;
    logf("dx9: Direct3DCreate9 intercepted; hooked IDirect3D9::CreateDevice");
  }
  return d3d;
}

}  // namespace

FARPROC wrap(const char* name, FARPROC real) {
  if (!std::strcmp(name, "Direct3DCreate9")) {
    g_real_create9 = reinterpret_cast<Create9Fn>(real);
    logf("dx9: game resolved Direct3DCreate9 - wrapping it");
    return reinterpret_cast<FARPROC>(&hk_create9);
  }
  return nullptr;
}

void uninstall() {
  if (g_device_vt && mem::readable(reinterpret_cast<void*>(g_device_vt), 64 * sizeof(void*))) {
    if (g_orig_end_scene) mem::write<void*>(g_device_vt + 42 * sizeof(void*), reinterpret_cast<void*>(g_orig_end_scene));
    if (g_orig_reset) mem::write<void*>(g_device_vt + 16 * sizeof(void*), reinterpret_cast<void*>(g_orig_reset));
  }
  if (g_d3d_vt && mem::readable(reinterpret_cast<void*>(g_d3d_vt), 17 * sizeof(void*)) && g_orig_create_device)
    mem::write<void*>(g_d3d_vt + 16 * sizeof(void*), reinterpret_cast<void*>(g_orig_create_device));
  g_orig_end_scene = nullptr;
  g_orig_reset = nullptr;
  g_orig_create_device = nullptr;
  if (g_renderer_ready) {
    ImGui_ImplDX9_Shutdown();
    g_renderer_ready = false;
  }
}

}  // namespace f3tm::overlay::dx9
