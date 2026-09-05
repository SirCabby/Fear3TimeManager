#include "dispatch.h"

#include <windows.h>

#include "config.h"
#include "game.h"
#include "log.h"
#include "mem.h"

#include <cstdio>
#include <cstring>

namespace f3tm::dispatch {
namespace {

using PeekMessageAFn = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
PeekMessageAFn g_orig_peek = nullptr;
DWORD g_main_thread = 0;  // the process's primary thread: DllMain(PROCESS_ATTACH) runs on it

Snapshot g_snap;
DWORD g_last_tick = 0;
bool g_tick_logged = false;
bool g_in_level_prev = false;

// Requests, written by the render thread, consumed here.
volatile LONG g_req_freeze = -1;      // -1 none, 0 resume, 1 freeze
volatile LONG g_req_set = -1;         // -1 none, else seconds
bool g_frozen = false;
float g_frozen_mission = 0.0f;     // the values held while frozen
float g_frozen_cumulative = -1.0f;

// Mission detection state, re-evaluated on the main thread.
DWORD g_last_mission_check = 0;
DWORD g_last_trace = 0;
uint32_t g_last_level_id = 0;
bool g_last_resolved = false;
bool g_table_dumped = false;
int g_auto_target = -1;
int g_auto_index = -1;
int g_auto_slot = -1;
int g_auto_difficulty = -1;
char g_auto_name[64] = {};
char g_auto_source[40] = {};

// The target for the detected mission: the ini value for the mission whose
// name matches the game's own level name, else the one at the same position in
// campaign order. 0 in the ini means "no target from the ini".
void refresh_mission(bool force) {
  const DWORD now = GetTickCount();
  if (!force && now - g_last_mission_check < 1000) return;
  g_last_mission_check = now;
  const game::MissionInfo m = game::current_mission();
  const bool changed = m.level_id != g_last_level_id || m.resolved != g_last_resolved;
  g_last_level_id = m.level_id;
  g_last_resolved = m.resolved;
  const int diff = game::difficulty();
  const bool diff_changed = diff != g_auto_difficulty;
  g_auto_target = -1;
  g_auto_index = -1;
  g_auto_slot = -1;
  g_auto_difficulty = diff;
  g_auto_name[0] = '\0';
  g_auto_source[0] = '\0';
  if (!m.resolved) {
    if (changed) {
      if (!m.profile_found) logf("mission: no PlayerProfilePC object found yet");
      else if (!m.table_found) logf("mission: no LevelListGlobalDataComponent object found yet");
      else if (!m.level_id || m.level_id == 0x811C9DC5) logf("mission: the profile carries no level id yet (0x%08X)", m.level_id);
      else logf("mission: level id 0x%08X is not in the level table (see the diagnostics dump)", m.level_id);
    }
    return;
  }
  std::snprintf(g_auto_name, sizeof(g_auto_name), "%s", m.name);
  g_auto_index = m.index;
  int slot = -1;
  for (int i = 0; i < config::kMissionCount; ++i)
    if (!_stricmp(m.name, config::kMissionNames[i])) { slot = i; break; }
  if (slot < 0 && m.index >= 0 && m.index < config::kMissionCount) slot = m.index;
  g_auto_slot = slot;
  // The target needs both the mission and the difficulty; pick the difficulty
  // column, else the game's own par field (set at the summary) fills in.
  if (slot >= 0 && diff >= 0) {
    const int t = config::target_for(slot, diff);
    if (t > 0) {
      g_auto_target = t;
      std::snprintf(g_auto_source, sizeof(g_auto_source), "Target%s[%s]", config::kMissionNames[slot],
                    config::kDifficultyNames[diff]);
    }
  }
  if (changed || diff_changed) {
    if (slot >= 0)
      logf("mission: \"%s\" (index %d, id 0x%08X) difficulty=%s -> %s = %d s", m.name, m.index, m.level_id,
           diff >= 0 ? config::kDifficultyNames[diff] : "unknown",
           g_auto_source[0] ? g_auto_source : "no target", g_auto_target);
    else
      logf("mission: \"%s\" (index %d, id 0x%08X) has no campaign slot", m.name, m.index, m.level_id);
  }
}

void tick() {
  const DWORD now = GetTickCount();
  if (now - g_last_tick < 16) return;  // the pump spins many times per frame
  g_last_tick = now;

  Snapshot s;
  s.main_thread = GetCurrentThreadId();
  s.game_ready = game::ready();
  if (!g_tick_logged) {
    g_tick_logged = true;
    logf("main-thread tick running on thread %lu", s.main_thread);
  }
  if (!s.game_ready) {
    g_snap = s;
    return;
  }
  s.managers_ready = game::context_ready();
  const int player = config::get().player_index;
  s.in_level = game::level_started(player);
  s.paused_menu = s.in_level && game::pause_menu_showing();

  if (s.in_level != g_in_level_prev) {
    logf("level %s (player %d)", s.in_level ? "started" : "ended", player);
    g_in_level_prev = s.in_level;
    if (!s.in_level && g_frozen) {
      logf("level ended - releasing the timer hold");
      g_frozen = false;
    }
    if (s.in_level) {
      refresh_mission(true);
      if (config::get().trace) game::dump_diagnostics("level start");
    }
  }
  if (!g_table_dumped && game::mission_detection_ready()) {
    // Once, as soon as the level table is readable: the record list is what a
    // log reader needs to map ids to missions.
    g_table_dumped = true;
    game::dump_diagnostics("level table available");
  }

  if (s.managers_ready) {
    // Requests first, so the readings below reflect them.
    const LONG set = InterlockedExchange(&g_req_set, -1);
    if (set >= 0) {
      game::set_mission_time(static_cast<float>(set));
      g_frozen_mission = static_cast<float>(set);
      g_frozen_cumulative = game::cumulative_time_f();
      logf("mission time set to %d s", set);
    }
    const LONG fr = InterlockedExchange(&g_req_freeze, -1);
    if (fr == 1 && !g_frozen) {
      g_frozen = true;
      g_frozen_mission = game::mission_time_f();
      g_frozen_cumulative = game::cumulative_time_f();
      logf("mission timer frozen at %.2f s", g_frozen_mission);
    } else if (fr == 0 && g_frozen) {
      g_frozen = false;
      logf("mission timer resumed at %.2f s", game::mission_time_f());
    }
    // The freeze: the game's timer keeps adding dt every frame (its own
    // StartMissionTimer refuses to restart in single player, so stopping it is
    // a one-way trip); instead the held values are re-asserted every tick.
    if (g_frozen && s.in_level) game::hold_time(g_frozen_mission, g_frozen_cumulative);
    s.timer_running = game::timer_running();
    s.mission_seconds = game::mission_time();
    refresh_mission(false);
    const int live_par = game::par_time();          // the game's field, set at mission end
    if (g_auto_target > 0) {
      s.par_seconds = g_auto_target;
      std::snprintf(s.target_source, sizeof(s.target_source), "%s", g_auto_source);
    } else if (live_par > 0) {
      s.par_seconds = live_par;
      std::snprintf(s.target_source, sizeof(s.target_source), "game par");
    } else {
      s.par_seconds = -1;
    }
    s.mission_known = g_auto_name[0] != '\0';
    s.mission_index = g_auto_index;
    s.difficulty = g_auto_difficulty;
    std::snprintf(s.mission_name, sizeof(s.mission_name), "%s", g_auto_name);
    if (config::get().trace) {
      static int last_par = -2;
      if (s.par_seconds != last_par) {
        logf("trace: par=%d (%s) running=%d locked=%d time=%.2f", s.par_seconds, s.target_source, s.timer_running,
             game::timer_locked(), game::mission_time_f());
        last_par = s.par_seconds;
      }
      if (s.in_level && now - g_last_trace > 15000) {
        g_last_trace = now;
        logf("trace: in level, profile level id 0x%08X mission \"%s\" index %d target %d (%s) time %.1f par field %d",
             g_last_level_id, g_auto_name, g_auto_index, s.par_seconds, s.target_source, game::mission_time_f(), live_par);
      }
    }
  }
  s.frozen = g_frozen;
  g_snap = s;
}

BOOL WINAPI hk_peek_message(LPMSG msg, HWND hwnd, UINT min, UINT max, UINT remove) {
  // Other threads pump messages too (the Steam overlay, worker windows); the
  // game runs on the primary thread, and so must everything that calls into it.
  if (GetCurrentThreadId() == g_main_thread) tick();
  return g_orig_peek(msg, hwnd, min, max, remove);
}

}  // namespace

bool install() {
  g_main_thread = GetCurrentThreadId();
  void* prev = mem::iat_hook(GetModuleHandleA(nullptr), "USER32.dll", "PeekMessageA",
                             reinterpret_cast<void*>(&hk_peek_message));
  if (!prev) {
    logf("ERROR: could not hook PeekMessageA in the exe's import table - no main-thread tick");
    return false;
  }
  g_orig_peek = reinterpret_cast<PeekMessageAFn>(prev);
  logf("dispatch: PeekMessageA import hooked (original %p)", prev);
  return true;
}

void uninstall() {
  if (!g_orig_peek) return;
  mem::iat_hook(GetModuleHandleA(nullptr), "USER32.dll", "PeekMessageA",
                reinterpret_cast<void*>(g_orig_peek));
  g_orig_peek = nullptr;
}

Snapshot snapshot() { return g_snap; }
void request_freeze(bool on) { InterlockedExchange(&g_req_freeze, on ? 1 : 0); }
void request_set_time(int seconds) { InterlockedExchange(&g_req_set, seconds < 0 ? 0 : seconds); }

}  // namespace f3tm::dispatch
