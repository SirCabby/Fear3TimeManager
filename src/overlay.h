#pragma once

#include <windows.h>

// The Dear ImGui panel and the machinery that gets it on screen. The renderer
// is captured through the exe's GetProcAddress import: the game resolves
// D3D11/DXGI/D3D9 entry points at runtime, and whichever API it ends up
// presenting frames with becomes the active backend.
namespace f3tm::overlay {

bool install();  // from DllMain (import-table patch only)
void uninstall();

enum class Backend { kNone, kDx11, kDx9 };

// Backend glue (called from the Present/EndScene hooks).
bool claim(Backend b);            // first backend to present wins
bool ensure_context(HWND hwnd);   // ImGui context + Win32 backend + WndProc, once
bool wants_draw();                // the visibility rule for this frame
void draw_panel();                // between ImGui::NewFrame() and ImGui::Render()
void shutdown_imgui();            // Win32 backend + context (renderer already gone)

// The two renderer backends.
namespace dx11 {
FARPROC wrap(const char* name, FARPROC real);
void uninstall();
}  // namespace dx11
namespace dx9 {
FARPROC wrap(const char* name, FARPROC real);
void uninstall();
}  // namespace dx9

}  // namespace f3tm::overlay
