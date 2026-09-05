#pragma once

namespace f3tm::config {

struct Settings {
  int toggle_key = 0x78;      // VK_F9: show/hide the timer HUD
  int pause_key = 0x79;       // VK_F10: pause/resume the mission timer
  bool show_on_start = true;  // HUD visible until hidden with the key
  int player_index = 0;       // local player slot used for the in-mission check
  float margin_x = 16.0f;     // HUD distance from the top-right corner, in pixels
  float margin_y = 16.0f;
  float scale = 1.0f;         // HUD font scale
  // Per-mission target times (seconds), campaign order. Defaults come from the
  // game's own world data; edit them in the ini. 0 for a mission means "use the
  // game's own par once it sets one".
  // Per-mission, per-difficulty target times in seconds, read from the game's
  // own world data (each world stores four "par" values, one per difficulty).
  // Rows: campaign order (see kMissionNames). Columns: difficulty index 0..3
  // (Recruit, Commando, Fearless, Insane). 0 = the mission has no par (Port).
  int mission_target[8][4] = {
      {900, 1200, 1320, 1500},   // Prison
      {1020, 1320, 1440, 1620},  // Slums
      {1200, 1500, 1620, 1800},  // Store
      {1500, 2100, 2400, 2700},  // Suburbs
      {1800, 2400, 2700, 3000},  // Tower
      {1200, 1800, 2100, 2400},  // Bridge
      {0, 0, 0, 0},              // Port (no par time)
      {720, 780, 840, 900},      // Ward
  };
  bool trace = false;         // verbose diagnostics
  bool always_show = false;   // debug: draw the HUD outside missions too
  // "Disable = overlay,dispatch,game" turns whole subsystems off so a fault can
  // be bisected to one of them.
  bool disable_overlay = false;
  bool disable_dispatch = false;
  bool disable_game = false;
};

const Settings& get();

// The 8 campaign missions, in order: the ini keys (Target<Name>) and the
// position each mission has in the game's level table.
constexpr int kMissionCount = 8;
extern const char* const kMissionNames[kMissionCount];

// The four difficulties, by the game's own index (0 = easiest). The labels are
// only for display; the values are keyed by the detected index, so a wrong
// label never affects which target is shown.
constexpr int kDifficultyCount = 4;
extern const char* const kDifficultyNames[kDifficultyCount];

// The target for a mission (0..7) and difficulty (0..3), from the ini/defaults.
// 0 means no target for that mission (e.g. Port).
int target_for(int mission, int difficulty);

// Fear3TimeManager.ini next to the DLL. Written with documented defaults the
// first time so the options are discoverable.
void load(const char* dir);
const char* dir();  // the DLL's directory, with a trailing separator

}  // namespace f3tm::config
