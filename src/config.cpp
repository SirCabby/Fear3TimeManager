#include "config.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "log.h"

namespace f3tm::config {

const char* const kMissionNames[kMissionCount] = {"Prison",  "Slums",  "Store", "Suburbs",
                                                  "Tower",   "Bridge", "Port",  "Ward"};
// Display-only, best guess at the menu order; correctness does not depend on it.
const char* const kDifficultyNames[kDifficultyCount] = {"Recruit", "Commando", "Fearless", "Insane"};
namespace {

Settings g_settings;
char g_path[MAX_PATH] = {};
char g_dir[MAX_PATH] = {};

bool truthy(const char* v) {
  return !_stricmp(v, "1") || !_stricmp(v, "on") || !_stricmp(v, "true") || !_stricmp(v, "yes");
}

void write_defaults() {
  FILE* f = std::fopen(g_path, "w");
  if (!f) return;
  std::fprintf(f,
               "# Fear3TimeManager\n"
               "#\n"
               "#   ToggleKey   = 0x78   ; virtual-key code that shows/hides the timer (0x78 = F9)\n"
               "#   PauseKey    = 0x79   ; virtual-key code that pauses/resumes the mission timer (0x79 = F10)\n"
               "#   ShowOnStart = 1      ; timer visible until hidden with ToggleKey\n"
               "#   PlayerIndex = 0      ; local player slot used for the in-mission check\n"
               "#   MarginX     = 16     ; distance from the right edge of the screen, in pixels\n"
               "#   MarginY     = 16     ; distance from the top edge of the screen, in pixels\n"
               "#   Scale       = 1.0    ; text scale of the timer\n"
               "#\n"

               "#   AlwaysShow  = 0      ; debug: draw the timer outside missions too\n"
               "#   Trace       = 0      ; verbose diagnostics in Fear3TimeManager.log\n"
               "#   Disable     =        ; comma list of subsystems to turn off: overlay,dispatch,game\n"
               "ToggleKey = 0x78\n"
               "PauseKey = 0x79\n"
               "ShowOnStart = 1\n"
               "PlayerIndex = 0\n"
               "MarginX = 16\n"
               "MarginY = 16\n"
               "Scale = 1.0\n"
               "# Per-mission target times in seconds, one line per mission, four values:\n"
               "# Recruit,Commando,Fearless,Insane. Read from the game's world data. 0 = no par.\n"
               "TargetPrison = 900,1200,1320,1500\n"
               "TargetSlums = 1020,1320,1440,1620\n"
               "TargetStore = 1200,1500,1620,1800\n"
               "TargetSuburbs = 1500,2100,2400,2700\n"
               "TargetTower = 1800,2400,2700,3000\n"
               "TargetBridge = 1200,1800,2100,2400\n"
               "TargetPort = 0,0,0,0\n"
               "TargetWard = 720,780,840,900\n"
               "AlwaysShow = 0\n"
               "Trace = 0\n"
               "Disable =\n");
  std::fclose(f);
}

}  // namespace

const Settings& get() { return g_settings; }
const char* dir() { return g_dir; }

int target_for(int mission, int difficulty) {
  if (mission < 0 || mission >= kMissionCount || difficulty < 0 || difficulty >= kDifficultyCount) return 0;
  return g_settings.mission_target[mission][difficulty];
}

void load(const char* dir) {
  std::snprintf(g_dir, sizeof(g_dir), "%s", dir);
  std::snprintf(g_path, sizeof(g_path), "%sFear3TimeManager.ini", dir);
  FILE* f = std::fopen(g_path, "r");
  if (!f) {
    write_defaults();
    logf("config: no %s - wrote one with the defaults", g_path);
    return;
  }
  char line[256];
  while (std::fgets(line, sizeof(line), f)) {
    char* p = line;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '#' || *p == ';' || *p == '[' || *p == '\n' || *p == '\r' || !*p) continue;
    char* eq = std::strchr(p, '=');
    if (!eq) continue;
    *eq = '\0';
    char* key = p;
    char* val = eq + 1;
    for (char* e = key + std::strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\t');) *--e = '\0';
    while (*val == ' ' || *val == '\t') ++val;
    for (char* e = val + std::strlen(val);
         e > val && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t');)
      *--e = '\0';

    if (!_stricmp(key, "ToggleKey")) {
      int v = static_cast<int>(std::strtol(val, nullptr, 0));
      if (v > 0 && v < 256) g_settings.toggle_key = v;
    } else if (!_stricmp(key, "PauseKey")) {
      int v = static_cast<int>(std::strtol(val, nullptr, 0));
      if (v > 0 && v < 256) g_settings.pause_key = v;
    } else if (!_stricmp(key, "ShowOnStart")) {
      g_settings.show_on_start = truthy(val);
    } else if (!_stricmp(key, "PlayerIndex")) {
      int v = static_cast<int>(std::strtol(val, nullptr, 0));
      if (v >= 0 && v < 16) g_settings.player_index = v;
    } else if (!_stricmp(key, "MarginX")) {
      g_settings.margin_x = static_cast<float>(std::strtod(val, nullptr));
    } else if (!_stricmp(key, "MarginY")) {
      g_settings.margin_y = static_cast<float>(std::strtod(val, nullptr));
    } else if (!_stricmp(key, "Scale")) {
      float v = static_cast<float>(std::strtod(val, nullptr));
      if (v >= 0.5f && v <= 4.0f) g_settings.scale = v;
    } else if (!_strnicmp(key, "Target", 6)) {
      int mi = -1;
      for (int i = 0; i < kMissionCount; ++i)
        if (!_stricmp(key + 6, kMissionNames[i])) { mi = i; break; }
      if (mi < 0) {
        logf("config: unknown key '%s' ignored", key);
      } else {
        // Value is up to four comma-separated seconds (Recruit,Commando,Fearless,Insane).
        int di = 0;
        for (char* tok = std::strtok(val, ", \t"); tok && di < kDifficultyCount; tok = std::strtok(nullptr, ", \t")) {
          int v = static_cast<int>(std::strtol(tok, nullptr, 0));
          if (v >= 0 && v <= 359999) g_settings.mission_target[mi][di] = v;
          ++di;
        }
      }
    } else if (!_stricmp(key, "AlwaysShow")) {
      g_settings.always_show = truthy(val);
    } else if (!_stricmp(key, "Trace")) {
      g_settings.trace = truthy(val);
    } else if (!_stricmp(key, "Disable")) {
      g_settings.disable_overlay = std::strstr(val, "overlay") != nullptr;
      g_settings.disable_dispatch = std::strstr(val, "dispatch") != nullptr;
      g_settings.disable_game = std::strstr(val, "game") != nullptr;
    } else {
      logf("config: unknown key '%s' ignored", key);
    }
  }
  std::fclose(f);
  logf("config: ToggleKey=0x%02X PauseKey=0x%02X ShowOnStart=%d PlayerIndex=%d Scale=%.2f AlwaysShow=%d Trace=%d",
       g_settings.toggle_key, g_settings.pause_key, g_settings.show_on_start, g_settings.player_index,
       g_settings.scale, g_settings.always_show, g_settings.trace);
  for (int i = 0; i < kMissionCount; ++i)
    logf("config: target %-8s Recruit=%d Commando=%d Fearless=%d Insane=%d", kMissionNames[i],
         g_settings.mission_target[i][0], g_settings.mission_target[i][1], g_settings.mission_target[i][2],
         g_settings.mission_target[i][3]);
}

}  // namespace f3tm::config
