// The overlay's vtable handling with the Steam overlay's way of hooking, outside the game:
//
//   mkdir -p tests/build
//   i686-w64-mingw32-g++ -std=c++20 -O2 -Wall -DWIN32_LEAN_AND_MEAN -static tests/test_adopt.cpp
//       -luser32 -lgdi32 -lole32 -o tests/build/test_adopt.exe
//   cp build/fmodex.dll "$GAME_DIR/fmodex_orig.dll" <dir>/      (a scratch folder)
//   F3TM_ADOPT=1 wine tests/build/test_adopt.exe 'Z:\<dir>\' dx11 overlay adopt
//   F3TM_ADOPT=1 wine tests/build/test_adopt.exe 'Z:\<dir>\' dx9 overlay adopt
//   wine tests/build/test_adopt.exe 'Z:\<dir>\' dx11 none class      (Wine's own way, as before)
//   wine tests/build/test_adopt.exe 'Z:\<dir>\' dx9 none class
//
// (one process per run: the proxy keeps its state; F3TM_ADOPT makes it take
// the Windows path under Wine). The exe loads the proxy the way the game's
// import does - on its primary thread, before any renderer exists - and
// resolves CreateDXGIFactory1 / D3D11CreateDevice / Direct3DCreate9 through
// GetProcAddress as the game does, so the proxy's import hook wraps them.
//
// "overlay" plays the Steam overlay on Windows (gameoverlayrenderer.dll,
// 2026-09-27): installed before the proxy, it hooks every swap chain (device)
// as it is made, by what the object's class vtable points to at that moment -
// here by writing its own function into that slot, which a caller of the slot
// meets the same way as the overlay's jump in the function - keeping ONE saved
// original per hook and skipping a slot it hooked already. The game makes its
// swap chain twice; with the proxy's function in the class's vtable (1.0.0)
// the second pass takes it for the original and the two call each other: the
// game died of a stack overflow on Windows. Here the overlay's hooks count
// their depth and give up past 4 (a "LOOP" failure), so the run survives to
// report it. With the fix each object for the test's window (made by its
// primary thread) must come back with a vtable of its own holding the proxy's
// hooks, the class's vtable must still hold only the overlay's, and every call
// must pass through the overlay exactly once; an object for another thread's
// window must keep its class's vtable. The proxy's log in <dir> says the rest:
// the panel's renderer came up (AlwaysShow = 1), and on exit the teardown is
// skipped ("process exiting").
#include <windows.h>
#include <d3d11.h>
#include <d3d9.h>
#include <dxgi.h>

#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;
HMODULE g_proxy = nullptr;
bool g_expect_adopt = false;

void check(bool ok, const char* what) {
  std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_failures;
}

bool in_proxy(const void* fn) {
  HMODULE owner = nullptr;
  return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(fn), &owner) &&
         owner == g_proxy;
}

void** vtable_of(const void* obj) { return *static_cast<void** const*>(obj); }

void write_slot(void** vt, int slot, void* fn) {
  DWORD old = 0;
  VirtualProtect(&vt[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
  vt[slot] = fn;
  VirtualProtect(&vt[slot], sizeof(void*), old, &old);
}

// --- the overlay -------------------------------------------------------------
struct OvHook {
  void* saved = nullptr;  // ONE original, whatever the slot held at the last pass
  int calls = 0;
  int depth = 0;
  bool looped = false;
};
OvHook g_present, g_resize, g_reset, g_present9;
void* g_real_create_swapchain = nullptr;
void* g_real_create_device = nullptr;
bool g_overlay = false;

template <typename Fn, typename... A>
HRESULT through(OvHook& h, A... a) {
  ++h.calls;
  if (h.depth >= 4) {
    h.looped = true;
    return E_FAIL;
  }
  ++h.depth;
  const HRESULT hr = reinterpret_cast<Fn>(h.saved)(a...);
  --h.depth;
  return hr;
}

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using Present9Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                                   IDirect3DDevice9**);

HRESULT STDMETHODCALLTYPE ov_present(IDXGISwapChain* sc, UINT sync, UINT flags) {
  return through<PresentFn>(g_present, sc, sync, flags);
}
HRESULT STDMETHODCALLTYPE ov_resize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
  return through<ResizeFn>(g_resize, sc, n, w, h, f, fl);
}
HRESULT STDMETHODCALLTYPE ov_reset(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* pp) { return through<ResetFn>(g_reset, d, pp); }
HRESULT STDMETHODCALLTYPE ov_present9(IDirect3DDevice9* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
  return through<Present9Fn>(g_present9, d, a, b, w, r);
}

// "Hooking vtable for swap chain": each slot, unless it is the overlay's already.
void ov_hook(void** vt, int slot, void* hook, OvHook& h) {
  if (vt[slot] == hook) return;
  h.saved = vt[slot];
  write_slot(vt, slot, hook);
}

HRESULT STDMETHODCALLTYPE ov_create_swapchain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
  const HRESULT hr = reinterpret_cast<CreateSwapChainFn>(g_real_create_swapchain)(f, dev, desc, out);
  if (SUCCEEDED(hr) && out && *out) {
    void** vt = vtable_of(*out);
    ov_hook(vt, 8, reinterpret_cast<void*>(&ov_present), g_present);
    ov_hook(vt, 13, reinterpret_cast<void*>(&ov_resize), g_resize);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE ov_create_device(IDirect3D9* d3d, UINT a, D3DDEVTYPE t, HWND w, DWORD fl, D3DPRESENT_PARAMETERS* pp,
                                           IDirect3DDevice9** out) {
  const HRESULT hr = reinterpret_cast<CreateDeviceFn>(g_real_create_device)(d3d, a, t, w, fl, pp, out);
  if (SUCCEEDED(hr) && out && *out) {
    void** vt = vtable_of(*out);
    ov_hook(vt, 16, reinterpret_cast<void*>(&ov_reset), g_reset);
    ov_hook(vt, 17, reinterpret_cast<void*>(&ov_present9), g_present9);
  }
  return hr;
}

// --- windows -------------------------------------------------------------------
HWND make_window(const char* title) {
  WNDCLASSA wc{};
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.lpszClassName = "F3CCTest";
  RegisterClassA(&wc);  // a second registration fails harmlessly
  HWND w = CreateWindowA("F3CCTest", title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 640, 480, nullptr, nullptr,
                         wc.hInstance, nullptr);
  return w;
}

void pump() {
  MSG m;
  while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&m);
    DispatchMessageA(&m);
  }
}

// Another thread's window: a tool's throwaway, not the game's.
HWND g_other = nullptr;
HANDLE g_other_ready = nullptr;
DWORD g_other_tid = 0;
DWORD WINAPI other_thread(LPVOID) {
  g_other = make_window("other thread");
  SetEvent(g_other_ready);
  MSG m;
  while (GetMessageA(&m, nullptr, 0, 0) > 0) {
    TranslateMessage(&m);
    DispatchMessageA(&m);
  }
  return 0;
}

// Through the exe's GetProcAddress import - which the proxy hooks - as the game resolves them.
__attribute__((noinline)) FARPROC resolve(const char* dll, const char* name) {
  return GetProcAddress(LoadLibraryA(dll), name);
}

// --- D3D11 ---------------------------------------------------------------------
using CreateFactory1Fn = HRESULT(WINAPI*)(REFIID, void**);
using CreateDeviceFn11 = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                          ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

IDXGISwapChain* make_chain(IDXGIFactory1* f, ID3D11Device* dev, HWND w) {
  DXGI_SWAP_CHAIN_DESC d{};
  d.BufferDesc.Width = 640;
  d.BufferDesc.Height = 480;
  d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = 1;
  d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  d.BufferCount = 1;
  d.OutputWindow = w;
  d.Windowed = TRUE;
  d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  IDXGISwapChain* sc = nullptr;
  const HRESULT hr = f->CreateSwapChain(dev, &d, &sc);
  if (FAILED(hr)) std::printf("  CreateSwapChain -> 0x%08lX\n", hr);
  return SUCCEEDED(hr) ? sc : nullptr;
}

// The game's swap chain: adopted (or, the class hook, the proxy's in the class
// vtable), frames through the overlay exactly once each, no loop.
void game_chain(const char* name, IDXGIFactory1* f, ID3D11Device* dev, HWND w, void** cls) {
  char what[160];
  IDXGISwapChain* sc = make_chain(f, dev, w);
  std::snprintf(what, sizeof(what), "%s made", name);
  check(sc != nullptr, what);
  if (!sc) return;
  void** vt = vtable_of(sc);
  if (g_expect_adopt) {
    std::snprintf(what, sizeof(what), "%s has a vtable of its own, Present/ResizeBuffers the proxy's", name);
    check(vt != cls && in_proxy(vt[8]) && in_proxy(vt[13]), what);
    std::snprintf(what, sizeof(what), "%s: the class's vtable holds no function of the proxy's", name);
    check(!in_proxy(cls[8]) && !in_proxy(cls[13]), what);
  } else {
    std::snprintf(what, sizeof(what), "%s shares the class's vtable, Present/ResizeBuffers the proxy's", name);
    check(vt == cls && in_proxy(cls[8]) && in_proxy(cls[13]), what);
  }
  if (g_overlay) {
    std::snprintf(what, sizeof(what), "%s: the overlay's saved Present/ResizeBuffers are not the proxy's", name);
    check(!in_proxy(g_present.saved) && !in_proxy(g_resize.saved), what);
  }
  const int before_p = g_present.calls, before_r = g_resize.calls;
  bool ok = true;
  for (int i = 0; i < 3; ++i) {
    pump();
    ok = SUCCEEDED(sc->Present(0, 0)) && ok;
  }
  std::snprintf(what, sizeof(what), "%s: 3 Presents succeed%s", name, g_overlay ? ", each through the overlay once" : "");
  check(ok && !g_present.looped && (!g_overlay || g_present.calls - before_p == 3), what);
  const HRESULT hr = sc->ResizeBuffers(0, 800, 600, DXGI_FORMAT_UNKNOWN, 0);
  std::snprintf(what, sizeof(what), "%s: ResizeBuffers succeeds%s (0x%08lX)", name, g_overlay ? ", through the overlay once" : "",
                hr);
  check(SUCCEEDED(hr) && !g_resize.looped && (!g_overlay || g_resize.calls - before_r == 1), what);
  pump();
  ok = SUCCEEDED(sc->Present(0, 0));
  std::snprintf(what, sizeof(what), "%s: a Present after the resize succeeds", name);
  check(ok && !g_present.looped, what);
  sc->Release();
}

int run_dx11() {
  if (g_overlay) {
    // The overlay is in before the proxy: its CreateSwapChain hook on the factory class.
    auto create = reinterpret_cast<CreateFactory1Fn>(GetProcAddress(LoadLibraryA("dxgi.dll"), "CreateDXGIFactory1"));
    IDXGIFactory1* f = nullptr;
    if (!create || FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f))) || !f) {
      std::printf("SKIP: no DXGI factory\n");
      return 0;
    }
    g_real_create_swapchain = vtable_of(f)[10];
    write_slot(vtable_of(f), 10, reinterpret_cast<void*>(&ov_create_swapchain));
    f->Release();
  }
  g_proxy = LoadLibraryA("fmodex.dll");
  check(g_proxy != nullptr, "proxy loaded");
  if (!g_proxy) return 1;
  auto create = reinterpret_cast<CreateFactory1Fn>(resolve("dxgi.dll", "CreateDXGIFactory1"));
  auto create_dev = reinterpret_cast<CreateDeviceFn11>(resolve("d3d11.dll", "D3D11CreateDevice"));
  check(create && in_proxy(reinterpret_cast<void*>(create)), "CreateDXGIFactory1 resolved to the proxy's wrapper");
  IDXGIFactory1* f = nullptr;
  if (!create || FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f))) || !f) return 1;
  check(in_proxy(vtable_of(f)[10]), "the factory's CreateSwapChain is the proxy's (in place)");
  IDXGIAdapter* adapter = nullptr;
  f->EnumAdapters(0, &adapter);
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  HRESULT hr = create_dev(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                          D3D11_SDK_VERSION, &dev, nullptr, &ctx);
  if (FAILED(hr) || !dev) {
    std::printf("SKIP: no D3D11 device (0x%08lX)\n", hr);
    return 0;
  }
  HWND w = make_window("game window");
  // The class's vtable, from a swap chain for another thread's window, which
  // is never adopted - and which must present through the class as it is.
  IDXGISwapChain* other = make_chain(f, dev, g_other);
  check(other != nullptr, "swap chain for another thread's window made");
  if (!other) return 1;
  void** cls = vtable_of(other);
  if (g_expect_adopt) {
    check(!in_proxy(cls[8]), "another thread's swap chain keeps its class's vtable (no hook of the proxy's)");
  }
  check(SUCCEEDED(other->Present(0, 0)) && !g_present.looped, "another thread's swap chain presents");

  game_chain("swap chain 1", f, dev, w, cls);
  game_chain("swap chain 2", f, dev, w, cls);  // the game makes its swap chain twice
  game_chain("swap chain 3", f, dev, w, cls);
  check(SUCCEEDED(other->Present(0, 0)) && !g_present.looped, "another thread's swap chain still presents");
  other->Release();
  ctx->Release();
  dev->Release();
  if (adapter) adapter->Release();
  f->Release();
  DestroyWindow(w);
  return 0;
}

// --- D3D9 ----------------------------------------------------------------------
using Create9Fn = IDirect3D9*(WINAPI*)(UINT);

IDirect3DDevice9* make_device(IDirect3D9* d3d, HWND w, D3DPRESENT_PARAMETERS* pp) {
  *pp = D3DPRESENT_PARAMETERS{};
  pp->Windowed = TRUE;
  pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp->BackBufferFormat = D3DFMT_UNKNOWN;
  pp->hDeviceWindow = w;
  IDirect3DDevice9* dev = nullptr;
  const HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, w, D3DCREATE_SOFTWARE_VERTEXPROCESSING, pp, &dev);
  if (FAILED(hr)) std::printf("  CreateDevice -> 0x%08lX\n", hr);
  return SUCCEEDED(hr) ? dev : nullptr;
}

bool frame(IDirect3DDevice9* dev) {
  pump();
  dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(20, 20, 40), 1.0f, 0);
  const bool ok = SUCCEEDED(dev->BeginScene()) && SUCCEEDED(dev->EndScene());
  return SUCCEEDED(dev->Present(nullptr, nullptr, nullptr, nullptr)) && ok;
}

void game_device(const char* name, IDirect3D9* d3d, HWND w, void** cls) {
  char what[160];
  D3DPRESENT_PARAMETERS pp;
  IDirect3DDevice9* dev = make_device(d3d, w, &pp);
  std::snprintf(what, sizeof(what), "%s made", name);
  check(dev != nullptr, what);
  if (!dev) return;
  void** vt = vtable_of(dev);
  if (!cls) cls = vt;  // no other device to compare with (adopt: checked below)
  if (g_expect_adopt) {
    std::snprintf(what, sizeof(what), "%s has a vtable of its own, EndScene/Reset the proxy's", name);
    check(vt != cls && in_proxy(vt[42]) && in_proxy(vt[16]), what);
    std::snprintf(what, sizeof(what), "%s: the class's vtable holds no function of the proxy's", name);
    check(!in_proxy(cls[42]) && !in_proxy(cls[16]), what);
  } else {
    std::snprintf(what, sizeof(what), "%s shares the class's vtable, EndScene/Reset the proxy's", name);
    check(vt == cls && in_proxy(cls[42]) && in_proxy(cls[16]), what);
  }
  if (g_overlay) {
    std::snprintf(what, sizeof(what), "%s: the overlay's saved Reset/Present are not the proxy's", name);
    check(!in_proxy(g_reset.saved) && !in_proxy(g_present9.saved), what);
  }
  const int before_p = g_present9.calls, before_r = g_reset.calls;
  bool ok = true;
  for (int i = 0; i < 3; ++i) ok = frame(dev) && ok;
  std::snprintf(what, sizeof(what), "%s: 3 frames succeed%s", name, g_overlay ? ", each Present through the overlay once" : "");
  check(ok && !g_present9.looped && (!g_overlay || g_present9.calls - before_p == 3), what);
  const HRESULT hr = dev->Reset(&pp);
  std::snprintf(what, sizeof(what), "%s: Reset succeeds%s (0x%08lX)", name, g_overlay ? ", through the overlay once" : "", hr);
  check(SUCCEEDED(hr) && !g_reset.looped && (!g_overlay || g_reset.calls - before_r == 1), what);
  std::snprintf(what, sizeof(what), "%s: a frame after the Reset succeeds", name);
  check(frame(dev) && !g_present9.looped, what);
  dev->Release();
}

int run_dx9() {
  if (g_overlay) {
    auto create = reinterpret_cast<Create9Fn>(GetProcAddress(LoadLibraryA("d3d9.dll"), "Direct3DCreate9"));
    IDirect3D9* d3d = create ? create(D3D_SDK_VERSION) : nullptr;
    if (!d3d) {
      std::printf("SKIP: no Direct3D 9\n");
      return 0;
    }
    g_real_create_device = vtable_of(d3d)[16];
    write_slot(vtable_of(d3d), 16, reinterpret_cast<void*>(&ov_create_device));
    d3d->Release();
  }
  g_proxy = LoadLibraryA("fmodex.dll");
  check(g_proxy != nullptr, "proxy loaded");
  if (!g_proxy) return 1;
  auto create = reinterpret_cast<Create9Fn>(resolve("d3d9.dll", "Direct3DCreate9"));
  check(create && in_proxy(reinterpret_cast<void*>(create)), "Direct3DCreate9 resolved to the proxy's wrapper");
  IDirect3D9* d3d = create ? create(D3D_SDK_VERSION) : nullptr;
  if (!d3d) return 1;
  check(in_proxy(vtable_of(d3d)[16]), "IDirect3D9's CreateDevice is the proxy's (in place)");
  HWND w = make_window("game window");
  D3DPRESENT_PARAMETERS pp;
  IDirect3DDevice9* other = make_device(d3d, g_other, &pp);
  check(other != nullptr, "device for another thread's window made");
  void** cls = other ? vtable_of(other) : nullptr;
  if (other && g_expect_adopt) check(!in_proxy(cls[42]) && !in_proxy(cls[16]), "another thread's device keeps its class's vtable");
  if (other) check(frame(other) && !g_present9.looped, "another thread's device presents");
  game_device("device 1", d3d, w, cls);
  game_device("device 2", d3d, w, cls);  // a second device: the overlay's second pass
  if (other) {
    check(frame(other) && !g_present9.looped, "another thread's device still presents");
    other->Release();
  }
  d3d->Release();
  DestroyWindow(w);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 5) {
    std::printf("usage: test_adopt.exe <dir with fmodex.dll + fmodex_orig.dll> dx11|dx9 overlay|none adopt|class\n");
    return 2;
  }
  const char* dir = argv[1];
  const bool dx9 = std::strcmp(argv[2], "dx9") == 0;
  g_overlay = std::strcmp(argv[3], "overlay") == 0;
  g_expect_adopt = std::strcmp(argv[4], "adopt") == 0;
  SetDllDirectoryA(dir);
  SetCurrentDirectoryA(dir);
  // The proxy's ini: the panel drawn on every frame, no game to look for.
  char ini[MAX_PATH];
  std::snprintf(ini, sizeof(ini), "%sFear3TimeManager.ini", dir);
  if (FILE* f = std::fopen(ini, "w")) {
    std::fputs("AlwaysShow = 1\nDisable = game\n", f);
    std::fclose(f);
  }
  g_other_ready = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  HANDLE t = CreateThread(nullptr, 0, other_thread, nullptr, 0, &g_other_tid);
  WaitForSingleObject(g_other_ready, 5000);
  std::printf("%s, %s, expecting %s:\n", dx9 ? "D3D9" : "D3D11", g_overlay ? "the Steam overlay's way of hooking" : "no overlay",
              g_expect_adopt ? "vtables of their own" : "the class hook");
  const int rc = dx9 ? run_dx9() : run_dx11();
  PostThreadMessageA(g_other_tid, WM_QUIT, 0, 0);
  WaitForSingleObject(t, 5000);
  if (g_present.looped || g_resize.looped || g_reset.looped || g_present9.looped)
    std::printf("  LOOP: the overlay's hook and the proxy's called each other (a stack overflow in the game)\n");
  std::printf("%s (%d failure(s))\n", g_failures || rc ? "FAILED" : "PASSED", g_failures);
  std::fflush(stdout);
  return g_failures || rc ? 1 : 0;  // ExitProcess: the proxy's DllMain sees a process exit
}
