#pragma once

#include <cstdint>

// The game side: finds the engine's script-facing class descriptors by
// signature (for the vtable slots their registrations encode), the live
// FearScoreMgr and MenuMgr objects by their vtables, and wraps the timer calls
// and fields the HUD needs. Nothing here is an absolute address.
namespace f3tm::game {

bool discover();          // signature scan; true when everything below is usable
bool ready();
bool context_ready();     // both managers have been found (a level is loaded)
void instance_loop();     // mod-thread loop that finds (and re-finds) the managers; never returns

// Only meant for the game's main thread (see dispatch.cpp).
bool pause_menu_showing();
bool level_started(int player);
float mission_time_f();       // seconds, -1 if unavailable
int mission_time();           // whole seconds, -1 if unavailable
float cumulative_time_f();    // the second timer the same update advances, -1 if unavailable
int par_time();               // the game's par field (set at mission end), <=0 until then
bool timer_running();
bool timer_locked();
void set_mission_time(float seconds);           // sets the mission timer and the cumulative timer
void hold_time(float mission, float cumulative);  // re-assert both values (the freeze)

// The current mission: the level id the player profile carries, resolved
// through the level-list global data to the mission's progression index and
// its internal name.
struct MissionInfo {
  bool profile_found = false;   // a PlayerProfilePC object exists
  bool table_found = false;     // the LevelListGlobalDataComponent exists
  uint32_t level_id = 0;        // 0 = none; 0x811C9DC5 = the empty-name hash (no level)
  bool resolved = false;        // level_id matched a level record
  int index = -1;               // nProgressionIndexValue (0-based campaign order)
  char name[64] = {};           // strMissionName
};
bool mission_detection_ready();
MissionInfo current_mission();

// The current difficulty index (0 = easiest ... 3 = hardest), or -1 if not
// available. Read from the engine's DifficultyComponent.
int difficulty();
bool difficulty_ready();
void dump_diagnostics(const char* why);   // profiles, the level table, the score manager fields

uintptr_t exe_base();
const char* status_text();

}  // namespace f3tm::game
