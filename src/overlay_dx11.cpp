// D3D11 backend: the game resolves CreateDXGIFactory1 / D3D11CreateDevice at
// runtime through GetProcAddress, so overlay.cpp hands us the real entry points
// and we return wrappers. From the factory we hook IDXGIFactory::CreateSwapChain
// (vtable slot 10); from the swap chain, IDXGISwapChain::Present (8) and
// ResizeBuffers (13). Under DXVK, as on Windows, these vtables are shared by
// every instance of the class, so hooking one hooks the game's.

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include "overlay.h"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "log.h"
#include "mem.h"

namespace f3tm::overlay::dx11 {
namespace {

// Spelled out rather than pulled from dxguid so the link stays trivial.
constexpr GUID kIID_ID3D11Device = {0xdb6f6ddb, 0xac77, 0x4e88, {0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40}};
constexpr GUID kIID_ID3D11Texture2D = {0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};

using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
using CreateSwapChainFn = HRESULT(__stdcall*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                              IDXGISwapChain**);
using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using CreateDeviceAndSwapChainFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                    const D3D_FEATURE_LEVEL*, UINT, UINT,
                                                    const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
                                                    ID3D11Device**, D3D_FEATURE_LEVEL*,
                                                    ID3D11DeviceContext**);

CreateFactoryFn g_real_factory = nullptr;   // CreateDXGIFactory
CreateFactoryFn g_real_factory1 = nullptr;  // CreateDXGIFactory1
CreateDeviceAndSwapChainFn g_real_create_dev_sc = nullptr;
CreateSwapChainFn g_orig_create_swapchain = nullptr;
PresentFn g_orig_present = nullptr;
ResizeBuffersFn g_orig_resize = nullptr;
uintptr_t g_factory_vt = 0;
uintptr_t g_swapchain_vt = 0;

IDXGISwapChain* g_swapchain = nullptr;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
HWND g_hwnd = nullptr;
bool g_renderer_ready = false;

HRESULT __stdcall hk_present(IDXGISwapChain* sc, UINT sync, UINT flags);
HRESULT __stdcall hk_resize(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags);

void capture_swapchain(IDXGISwapChain* sc, HWND hwnd) {
  if (!sc) return;
  auto vt = mem::read<uintptr_t>(reinterpret_cast<uintptr_t>(sc));
  if (!g_orig_present) {
    g_orig_present = reinterpret_cast<PresentFn>(mem::hook_vtable(vt, 8, reinterpret_cast<void*>(&hk_present)));
    g_orig_resize = reinterpret_cast<ResizeBuffersFn>(mem::hook_vtable(vt, 13, reinterpret_cast<void*>(&hk_resize)));
    g_swapchain_vt = vt;
    logf("dx11: swap chain %p captured (hwnd=%p); hooked Present/ResizeBuffers", sc, hwnd);
  }
  g_swapchain = sc;
  if (hwnd) g_hwnd = hwnd;
}

bool ensure_renderer(IDXGISwapChain* sc) {
  if (g_renderer_ready) return true;
  if (!g_device) {
    if (FAILED(sc->GetDevice(kIID_ID3D11Device, reinterpret_cast<void**>(&g_device))) || !g_device) {
      logf("ERROR: dx11: swap chain has no ID3D11Device");
      return false;
    }
    g_device->GetImmediateContext(&g_context);
  }
  if (!g_hwnd) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (SUCCEEDED(sc->GetDesc(&desc))) g_hwnd = desc.OutputWindow;
  }
  if (!ensure_context(g_hwnd)) return false;
  if (!ImGui_ImplDX11_Init(g_device, g_context)) {
    logf("ERROR: dx11: ImGui DX11 backend init failed");
    return false;
  }
  g_renderer_ready = true;
  return true;
}

bool ensure_rtv(IDXGISwapChain* sc) {
  if (g_rtv) return true;
  ID3D11Texture2D* back = nullptr;
  if (FAILED(sc->GetBuffer(0, kIID_ID3D11Texture2D, reinterpret_cast<void**>(&back))) || !back) return false;
  HRESULT hr = g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
  back->Release();
  if (FAILED(hr)) {
    logf("ERROR: dx11: CreateRenderTargetView failed (0x%08lX)", hr);
    g_rtv = nullptr;
    return false;
  }
  return true;
}

HRESULT __stdcall hk_present(IDXGISwapChain* sc, UINT sync, UINT flags) {
  if (claim(Backend::kDx11) && ensure_renderer(sc) && wants_draw() && ensure_rtv(sc)) {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    draw_panel();
    ImGui::Render();
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  }
  return g_orig_present(sc, sync, flags);
}

HRESULT __stdcall hk_resize(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
  if (g_rtv) {
    g_rtv->Release();
    g_rtv = nullptr;
  }
  if (g_renderer_ready) ImGui_ImplDX11_InvalidateDeviceObjects();
  HRESULT hr = g_orig_resize(sc, count, w, h, fmt, flags);
  logf("dx11: ResizeBuffers(%ux%u) -> 0x%08lX", w, h, hr);
  return hr;
}

HRESULT __stdcall hk_create_swapchain(IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                      IDXGISwapChain** out) {
  HRESULT hr = g_orig_create_swapchain(self, device, desc, out);
  if (SUCCEEDED(hr) && out && *out) capture_swapchain(*out, desc ? desc->OutputWindow : nullptr);
  else logf("dx11: CreateSwapChain -> 0x%08lX", hr);
  return hr;
}

void hook_factory(void* factory) {
  if (!factory || g_orig_create_swapchain) return;
  auto vt = mem::read<uintptr_t>(reinterpret_cast<uintptr_t>(factory));
  g_orig_create_swapchain = reinterpret_cast<CreateSwapChainFn>(
      mem::hook_vtable(vt, 10, reinterpret_cast<void*>(&hk_create_swapchain)));
  g_factory_vt = vt;
  logf("dx11: DXGI factory %p intercepted; hooked CreateSwapChain", factory);
}

HRESULT WINAPI hk_create_factory(REFIID riid, void** out) {
  HRESULT hr = g_real_factory(riid, out);
  if (SUCCEEDED(hr) && out) hook_factory(*out);
  return hr;
}

HRESULT WINAPI hk_create_factory1(REFIID riid, void** out) {
  HRESULT hr = g_real_factory1(riid, out);
  if (SUCCEEDED(hr) && out) hook_factory(*out);
  return hr;
}

HRESULT WINAPI hk_create_dev_sc(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE sw, UINT flags,
                                const D3D_FEATURE_LEVEL* levels, UINT nlevels, UINT sdk,
                                const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** sc,
                                ID3D11Device** dev, D3D_FEATURE_LEVEL* level,
                                ID3D11DeviceContext** ctx) {
  HRESULT hr = g_real_create_dev_sc(adapter, type, sw, flags, levels, nlevels, sdk, desc, sc, dev, level, ctx);
  if (SUCCEEDED(hr) && sc && *sc) capture_swapchain(*sc, desc ? desc->OutputWindow : nullptr);
  return hr;
}

}  // namespace

FARPROC wrap(const char* name, FARPROC real) {
  if (!std::strcmp(name, "CreateDXGIFactory1")) {
    g_real_factory1 = reinterpret_cast<CreateFactoryFn>(real);
    logf("dx11: game resolved CreateDXGIFactory1 - wrapping it");
    return reinterpret_cast<FARPROC>(&hk_create_factory1);
  }
  if (!std::strcmp(name, "CreateDXGIFactory")) {
    g_real_factory = reinterpret_cast<CreateFactoryFn>(real);
    logf("dx11: game resolved CreateDXGIFactory - wrapping it");
    return reinterpret_cast<FARPROC>(&hk_create_factory);
  }
  if (!std::strcmp(name, "D3D11CreateDeviceAndSwapChain")) {
    g_real_create_dev_sc = reinterpret_cast<CreateDeviceAndSwapChainFn>(real);
    logf("dx11: game resolved D3D11CreateDeviceAndSwapChain - wrapping it");
    return reinterpret_cast<FARPROC>(&hk_create_dev_sc);
  }
  if (!std::strcmp(name, "D3D11CreateDevice")) logf("dx11: game resolved D3D11CreateDevice");
  return nullptr;
}

void uninstall() {
  if (g_swapchain_vt && mem::readable(reinterpret_cast<void*>(g_swapchain_vt), 16 * sizeof(void*))) {
    if (g_orig_present) mem::write<void*>(g_swapchain_vt + 8 * sizeof(void*), reinterpret_cast<void*>(g_orig_present));
    if (g_orig_resize) mem::write<void*>(g_swapchain_vt + 13 * sizeof(void*), reinterpret_cast<void*>(g_orig_resize));
  }
  if (g_factory_vt && mem::readable(reinterpret_cast<void*>(g_factory_vt), 12 * sizeof(void*)) && g_orig_create_swapchain)
    mem::write<void*>(g_factory_vt + 10 * sizeof(void*), reinterpret_cast<void*>(g_orig_create_swapchain));
  g_orig_present = nullptr;
  g_orig_resize = nullptr;
  g_orig_create_swapchain = nullptr;
  if (g_renderer_ready) {
    ImGui_ImplDX11_Shutdown();
    g_renderer_ready = false;
  }
  if (g_rtv) {
    g_rtv->Release();
    g_rtv = nullptr;
  }
  if (g_context) {
    g_context->Release();
    g_context = nullptr;
  }
  if (g_device) {
    g_device->Release();
    g_device = nullptr;
  }
}

}  // namespace f3tm::overlay::dx11
