#include "overlay.h"

#include <cstdio>
#include <cstring>

#include "config.h"
#include "dispatch.h"
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "adopt.h"
#include "log.h"
#include "mem.h"
#include "version.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace f3tm::overlay {
namespace {

using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
GetProcAddressFn g_orig_gpa = nullptr;

Backend g_backend = Backend::kNone;
HWND g_hwnd = nullptr;
WNDPROC g_orig_wndproc = nullptr;
bool g_context_ready = false;
bool g_visible = false;
bool g_interactive = false;  // the pause menu is up: controls shown, input routed to ImGui
bool g_user_hidden = false;  // the toggle key
char g_time_input[16] = {};
bool g_time_error = false;

FARPROC WINAPI hk_get_proc_address(HMODULE module, LPCSTR name) {
  FARPROC real = g_orig_gpa(module, name);
  if (!real || !name || !HIWORD(reinterpret_cast<uintptr_t>(name))) return real;
  if (FARPROC w = dx11::wrap(name, real)) return w;
  if (FARPROC w = dx9::wrap(name, real)) return w;
  return real;
}

LRESULT CALLBACK hk_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  const dispatch::Snapshot s = dispatch::snapshot();
  const bool in_mission = s.in_level || config::get().always_show;
  // F10 (and Alt-combinations) arrive as WM_SYSKEYDOWN rather than WM_KEYDOWN.
  if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !(lp & (1 << 30)) && in_mission) {  // ignore auto-repeat
    if (static_cast<int>(wp) == config::get().toggle_key) {
      g_user_hidden = !g_user_hidden;
      logf("timer %s by the toggle key", g_user_hidden ? "hidden" : "shown");
      return 0;
    }
    if (static_cast<int>(wp) == config::get().pause_key) {
      dispatch::request_freeze(!s.frozen);
      logf("%s requested by the pause key", s.frozen ? "resume" : "freeze");
      return 0;
    }
  }
  // Input only reaches ImGui while the game's pause menu is up: that is when
  // the HUD shows its controls and the cursor is free. During play the HUD is
  // drawn only, so the game keeps every key and mouse event.
  if (g_interactive && g_context_ready) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse && msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) return 0;
    if (io.WantTextInput && msg >= WM_KEYFIRST && msg <= WM_KEYLAST && wp != VK_ESCAPE) return 0;
  }
  return CallWindowProcA(g_orig_wndproc, hwnd, msg, wp, lp);
}

void format_time(int seconds, char* out, size_t n) {
  if (seconds < 0) {
    std::snprintf(out, n, "--:--");
    return;
  }
  const int h = seconds / 3600, m = (seconds / 60) % 60, sec = seconds % 60;
  if (h > 0) std::snprintf(out, n, "%d:%02d:%02d", h, m, sec);
  else std::snprintf(out, n, "%02d:%02d", m, sec);
}

// "mm:ss", "h:mm:ss" or plain seconds -> seconds; -1 if unparseable.
int parse_time(const char* text) {
  int parts[3] = {0, 0, 0};
  int count = 0;
  const char* p = text;
  while (*p == ' ') ++p;
  if (!*p) return -1;
  while (*p && count < 3) {
    if (*p < '0' || *p > '9') return -1;
    int v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    parts[count++] = v;
    if (*p == ':') ++p;
    else break;
  }
  while (*p == ' ') ++p;
  if (*p) return -1;
  if (count == 1) return parts[0];
  if (count == 2) return parts[0] * 60 + parts[1];
  return parts[0] * 3600 + parts[1] * 60 + parts[2];
}

}  // namespace

bool claim(Backend b) {
  if (g_backend == Backend::kNone) {
    g_backend = b;
    logf("overlay: %s is the active renderer", b == Backend::kDx11 ? "D3D11" : "D3D9");
  }
  return g_backend == b;
}

bool ensure_context(HWND hwnd) {
  if (g_context_ready) return true;
  if (!hwnd) return false;
  g_hwnd = hwnd;

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  // Window position/size and column layout persist beside the DLL, under the
  // mod's own name rather than a stray imgui.ini in the game folder.
  static char ini_path[MAX_PATH] = {};
  std::snprintf(ini_path, sizeof(ini_path), "%sFear3TimeManager.imgui.ini", config::dir());
  io.IniFilename = ini_path;
    ImGui::StyleColorsDark();
  ImGui::GetStyle().WindowRounding = 4.0f;

  if (!ImGui_ImplWin32_Init(hwnd)) {
    logf("ERROR: ImGui Win32 backend init failed");
    ImGui::DestroyContext();
    return false;
  }
  g_orig_wndproc = reinterpret_cast<WNDPROC>(
      SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hk_wndproc)));
  g_user_hidden = !config::get().show_on_start;
  g_context_ready = true;
  logf("overlay ready (hwnd=%p, render thread %lu)", hwnd, GetCurrentThreadId());
  return true;
}

bool wants_draw() {
  const dispatch::Snapshot s = dispatch::snapshot();
  const bool in_mission = s.in_level || config::get().always_show;
  if (!in_mission) g_user_hidden = !config::get().show_on_start;  // next mission starts from the default
  g_interactive = in_mission && s.paused_menu;
  // The HUD hides with the key; the controls still come up while paused so it
  // can be shown again without the key.
  g_visible = g_context_ready && in_mission && (!g_user_hidden || g_interactive);
  ImGui::GetIO().MouseDrawCursor = g_interactive && g_visible;
  return g_visible;
}

void draw_panel() {
  const dispatch::Snapshot s = dispatch::snapshot();
  const ImGuiIO& io = ImGui::GetIO();
  const config::Settings& cfg = config::get();

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                           ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus;
  if (!g_interactive) flags |= ImGuiWindowFlags_NoInputs;
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - cfg.margin_x, cfg.margin_y), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
  ImGui::SetNextWindowBgAlpha(g_interactive ? 0.85f : 0.55f);
  if (!ImGui::Begin("Fear3TimeManager", nullptr, flags)) {
    ImGui::End();
    return;
  }
  ImGui::SetWindowFontScale(cfg.scale);

  char played[24], target[24];
  format_time(s.mission_seconds, played, sizeof(played));
  format_time(s.par_seconds > 0 ? s.par_seconds : -1, target, sizeof(target));  // 0 = the game has not set one
  const bool over = s.par_seconds > 0 && s.mission_seconds > s.par_seconds;
  if (!s.managers_ready) {
    ImGui::TextDisabled("--:-- / --:--");
  } else {
    ImGui::TextColored(over ? ImVec4(1.0f, 0.45f, 0.35f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%s", played);
    ImGui::SameLine(0, 0);
    ImGui::TextDisabled(" / %s", target);
    if (s.frozen) {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "(paused)");
    }
  }

  if (g_interactive) {
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    if (ImGui::Button(s.frozen ? "Resume timer" : "Pause timer", ImVec2(110, 0)))
      dispatch::request_freeze(!s.frozen);
    ImGui::SameLine();
    if (ImGui::Button(g_user_hidden ? "Show HUD" : "Hide HUD", ImVec2(90, 0))) g_user_hidden = !g_user_hidden;

    ImGui::SetNextItemWidth(90);
    const bool entered = ImGui::InputTextWithHint("##time", "mm:ss", g_time_input, sizeof(g_time_input),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Set played") || entered) {
      const int secs = parse_time(g_time_input);
      if (secs >= 0) { dispatch::request_set_time(secs); g_time_input[0] = '\0'; g_time_error = false; }
      else g_time_error = true;
    }
    if (g_time_error) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "Enter mm:ss, h:mm:ss or seconds");

    // Where the target comes from: the per-mission per-difficulty table, or the
    // game's own par (set at the mission summary), or nothing yet.
    const char* diff = (s.difficulty >= 0 && s.difficulty < config::kDifficultyCount)
                           ? config::kDifficultyNames[s.difficulty] : "?";
    if (s.mission_known && s.target_source[0])
      ImGui::TextDisabled("Mission %d (%s)  target: %s", s.mission_index + 1, diff, s.target_source);
    else if (s.mission_known)
      ImGui::TextDisabled("Mission %d (%s)  no target set", s.mission_index + 1, diff);
    else
      ImGui::TextDisabled("Mission: detecting...");
    // A pause survives level changes, so the held time can belong to an earlier
    // mission; say so rather than let a stale number sit against a new target.
    if (s.frozen && s.frozen_mission_index >= 0 && s.mission_index >= 0 &&
        s.frozen_mission_index != s.mission_index)
      ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Paused since mission %d - \"Set played\" re-bases it",
                         s.frozen_mission_index + 1);
    ImGui::TextDisabled("F%d hide/show  |  F%d pause/resume", cfg.toggle_key - 0x6F, cfg.pause_key - 0x6F);
    if (!s.timer_running && !s.frozen) ImGui::TextDisabled("(the game has its timer stopped right now)");
  }
  ImGui::End();
}

void shutdown_imgui() {
  if (!g_context_ready) return;
  ImGui_ImplWin32_Shutdown();
  if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
  g_context_ready = false;
  g_visible = false;
}

bool install() {
  adopt::init();  // on the game's primary thread: the one that makes its window
  void* prev = mem::iat_hook(GetModuleHandleA(nullptr), "KERNEL32.dll", "GetProcAddress",
                             reinterpret_cast<void*>(&hk_get_proc_address));
  if (!prev) {
    logf("ERROR: could not hook GetProcAddress in the exe's import table - no overlay");
    return false;
  }
  g_orig_gpa = reinterpret_cast<GetProcAddressFn>(prev);
  logf("overlay: GetProcAddress import hooked (original %p)", prev);
  return true;
}

void uninstall() {
  // Window procedure first: a message arriving after our image is gone would
  // jump into freed memory.
  if (g_orig_wndproc && g_hwnd && IsWindow(g_hwnd)) {
    SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_orig_wndproc));
    g_orig_wndproc = nullptr;
  }
  dx11::uninstall();
  dx9::uninstall();
  shutdown_imgui();
  if (g_orig_gpa) {
    mem::iat_hook(GetModuleHandleA(nullptr), "KERNEL32.dll", "GetProcAddress",
                  reinterpret_cast<void*>(g_orig_gpa));
    g_orig_gpa = nullptr;
  }
  logf("overlay hooks removed");
}

}  // namespace f3tm::overlay
