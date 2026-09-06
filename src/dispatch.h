#pragma once

// The main-thread tick. The game's own script calls into the score manager run
// on the thread that pumps window messages, so the mod's calls do too: an
// import-table hook on PeekMessageA gives a callback on that thread once per
// pump, and everything that talks to the game happens from there.
namespace f3tm::dispatch {

bool install();  // from DllMain (import-table patch only); records the primary thread
void uninstall();

struct Snapshot {
  bool game_ready = false;
  bool managers_ready = false;
  bool in_level = false;
  bool paused_menu = false;     // the game's pause menu is up
  bool timer_running = false;   // the game's own flag
  bool frozen = false;          // the mod is holding the timer
  int frozen_mission_index = -1;  // the mission the hold was taken in (see mission_index)
  int mission_seconds = -1;     // played time; -1 = unknown
  int par_seconds = -1;         // target time; -1 = unknown
  unsigned long main_thread = 0;
  // Mission detection (see game.h): the ini key the target came from, or the
  // game's own par field, or nothing.
  bool mission_known = false;
  int mission_index = -1;       // 0-based campaign order from the level list
  int difficulty = -1;          // 0..3 (Recruit..Insane), -1 if unknown
  char mission_name[64] = {};   // the level's internal name
  char target_source[40] = {};  // "TargetPrison[Fearless]", "game par", ""
};
Snapshot snapshot();

// Requests from the overlay (applied on the next tick, on the main thread).
void request_freeze(bool on);            // pause (true) or resume (false) the mission timer
void request_set_time(int seconds);      // set the played time

}  // namespace f3tm::dispatch
