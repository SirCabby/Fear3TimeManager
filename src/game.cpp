#include "game.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "config.h"
#include "log.h"
#include "mem.h"

namespace f3tm::game {
namespace {

// The engine registers Lua-visible methods through Despair::Reflection method
// proxies, keyed on the method-name string. Two code shapes:
//   Pattern A: B8 <impl> 89 47 20 33 C0 C7 47 10 <name> 89 5F 14 89 77 18 C7 07 <proxyVtbl>
//              (registering function's prologue: 83 EC ?? 53 55 56 33 F6 57 BB <static>)
//   Pattern B: B8 <fn> 50 68 <name> E8 rel32 83 C4 0C 50 B9 <static> E8
// The statics are class descriptors, not instances, so the wrappers cannot be
// called through them; the mod reads them for the vtable slots they encode and
// calls the live managers directly, finding those by their RTTI vtables.
// (HasPlayerStartedLevel is the one wrapper that is called: it uses no `this`.)
using LevelFn = bool(__stdcall*)(int player);
using ThisCall0 = void*(__fastcall*)(void* self, void* edx);
using ThisCall0B = bool(__fastcall*)(void* self, void* edx);

uintptr_t g_base = 0;
mem::Range g_text;
std::vector<mem::Range> g_data;

uintptr_t g_fearscore_static = 0;
uintptr_t g_game_static = 0;
uintptr_t g_pause_impl = 0;
LevelFn g_level = nullptr;

uint32_t g_off_menumgr = 0;    // [ctx+0x238] -> MenuMgr subobject (documentation only)
int g_slot_pause = -1;         // MenuMgr sub-vtbl[69]
int g_slot_start = -1;         // FearScoreMgr vtbl[54] StartMissionTimer
int g_slot_stop = -1;          // vtbl[55] StopMissionTimer
int g_slot_reset = -1;         // vtbl[56] ResetMissionTimer
int g_slot_time = -1;          // vtbl[57] GetMissionTime -> int
int g_slot_par = -1;           // vtbl[58] GetParTime -> int

uint32_t g_off_timer = 0;      // FearScoreMgr+0x88  float  mission time
uint32_t g_off_cumul = 0;      // +0x8C  float  cumulative timer (advanced with the mission timer)
uint32_t g_off_running = 0;    // +0x9A  bool
uint32_t g_off_locked = 0;     // +0x9B  bool
uint32_t g_off_par = 0;        // +0xC0  int    par time

std::vector<mem::VtableInfo> g_fsm_vts, g_mm_vts;
uintptr_t g_fsm_vt0 = 0, g_mm_vt0 = 0, g_mm_sub_off = 0, g_mm_sub_vt = 0;
volatile uintptr_t g_fsm_obj = 0;
volatile uintptr_t g_mm_obj = 0;

// --- mission detection --------------------------------------------------------
// Where the current mission comes from, all read from the function bytes:
//  * PlayerProfileMgr's SetCurrentLevel(levelId, flag) (a vtable function of the
//    manager, found by 8B 01 8B 54 24 ?? 8B 80 <slot*4> 52 FF D0) stores the id
//    on the primary profile through one PlayerProfilePC slot; that setter is
//    `mov eax,[esp+4]; mov [ecx+X],eax; ret 4` and the getter right after it
//    `mov eax,[ecx+X]; ret`, so the current mission is an int at profile+X
//    (0x11C in build 3576).
//  * Next to it (0x118) sits the profile's progression: the furthest level
//    reached, which GameScriptGame::SetCurrentLevelId's core only ever advances
//    (8B 11 8B 82 <slot*4> FF D0 ... 8B 42 0C ...). It is read for the log only;
//    a profile that finished the game reports the last mission there.
//  * The id is a key into ILevelListGlobalDataComponent's levelMap (its slot 3
//    looks an id up and returns the progression index; slot 33 does the reverse).
//    The map keeps an intrusive list of nodes {next, ..., +0x10 LevelDescriptor};
//    the descriptor holds nProgressionIndexValue at +0xC, strMissionName
//    (std::string) at +0x10 and the level id at +0x48 (node+0x58). Every offset
//    is parsed out of those two functions; only the name offset comes from the
//    property accessor (lea eax,[ecx+0x10]) and is checked by hashing the name.
constexpr const char* kRttiProfile = ".?AVPlayerProfilePC@Despair@@";
constexpr const char* kRttiProfileMgr = ".?AVPlayerProfileMgr@Despair@@";
constexpr const char* kRttiProfileMgrPC = ".?AVPlayerProfilePCMgr@Despair@@";
constexpr const char* kRttiLevelList = ".?AVLevelListGlobalDataComponent@Despair@@";
constexpr uint32_t kEmptyHash = 0x811C9DC5;  // FNV offset basis = hash of ""
constexpr int kMaxProfiles = 8;

std::vector<mem::VtableInfo> g_pp_vts, g_ll_vts;
uintptr_t g_pp_vt0 = 0, g_ll_vt0 = 0;
uint32_t g_ll_iface_off = 0;        // subobject with the ILevelListGlobalDataComponent vtable
int g_slot_level_id = -1;           // PlayerProfilePC slot that returns the current level id
int g_slot_progress = -1;           // PlayerProfilePC slot that returns the furthest level reached
uint32_t g_off_profile_level = 0;   // profile+X: the current level id
uint32_t g_off_profile_progress = 0;  // profile+X: the progression id (log only)
uint32_t g_off_map = 0;             // iface+X: the levelMap
uint32_t g_off_sentinel[3] = {};    // iface+X: the three list sentinels (levelMap first)
uint32_t g_off_desc = 0;            // node+X: the LevelDescriptor
uint32_t g_off_node_index = 0;      // node+X: nProgressionIndexValue
uint32_t g_off_node_id = 0;         // node+X: level id
uint32_t g_off_desc_name = 0x10;    // descriptor+X: strMissionName (std::string)
bool g_mission_layout = false;

uintptr_t g_pp_objs[kMaxProfiles] = {};
volatile LONG g_pp_count = 0;
volatile uintptr_t g_ll_obj = 0;

// The difficulty component: the current difficulty is a small int (0..3) read
// through its interface subobject's first getter (mov eax,[ecx+X]; ret).
constexpr const char* kRttiDifficulty = ".?AVDifficultyComponent@Despair@@";
std::vector<mem::VtableInfo> g_diff_vts;
uintptr_t g_diff_vt0 = 0;
uint32_t g_off_difficulty = 0;   // component + this = the difficulty int
bool g_diff_layout = false;
volatile uintptr_t g_diff_obj = 0;

bool g_ready = false;
char g_status[160] = "not started";

constexpr const char* kRttiFearScoreMgr = ".?AVFearScoreMgr@Despair@@";
constexpr const char* kRttiMenuMgr = ".?AVMenuMgr@Despair@@";

void set_status(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(g_status, sizeof(g_status), fmt, args);
  va_end(args);
}

uintptr_t rva(uintptr_t a) { return a ? a - g_base : 0; }

mem::Pattern with_imm32(const char* head, uint32_t imm, const char* tail = "") {
  mem::Pattern p = mem::parse_pattern(head);
  for (int i = 0; i < 4; ++i) p.bytes.push_back(static_cast<int16_t>((imm >> (8 * i)) & 0xFF));
  mem::Pattern t = mem::parse_pattern(tail);
  p.bytes.insert(p.bytes.end(), t.bytes.begin(), t.bytes.end());
  return p;
}

bool find_a(const char* name, uintptr_t* impl, uintptr_t* site) {
  const uintptr_t s = mem::find_cstring(g_data, name);
  if (!s) { logf("ERROR: name string '%s' not found in the exe", name); return false; }
  const mem::Pattern p = with_imm32("B8 ?? ?? ?? ?? 89 47 20 33 C0 C7 47 10", static_cast<uint32_t>(s));
  const uintptr_t a = mem::find_pattern(g_text, p);
  if (!a) { logf("ERROR: registration of '%s' not found", name); return false; }
  const uintptr_t fn = mem::read<uint32_t>(a + 1);
  if (!g_text.contains(fn)) { logf("ERROR: '%s' impl outside .text", name); return false; }
  *impl = fn; *site = a;
  return true;
}

bool find_owner_a(uintptr_t site, uintptr_t* out) {
  const mem::Pattern prologue = mem::parse_pattern("83 EC ?? 53 55 56 33 F6 57 BB");
  const mem::Range whole = mem::module_range(reinterpret_cast<HMODULE>(g_base));
  const uintptr_t low = site > g_text.begin + 0x4000 ? site - 0x4000 : g_text.begin;
  for (uintptr_t a = site; a >= low; --a) {
    const mem::Range r{a, a + prologue.bytes.size()};
    if (mem::find_pattern(r, prologue) != a) continue;
    const uintptr_t obj = mem::read<uint32_t>(a + 10);
    if (!whole.contains(obj) || g_text.contains(obj)) continue;
    *out = obj; return true;
  }
  return false;
}

bool find_b(const char* name, uintptr_t* fn, uintptr_t* owner) {
  const uintptr_t s = mem::find_cstring(g_data, name);
  if (!s) { logf("ERROR: name string '%s' not found in the exe", name); return false; }
  const mem::Pattern p = with_imm32("B8 ?? ?? ?? ?? 50 68", static_cast<uint32_t>(s),
                                    "E8 ?? ?? ?? ?? 83 C4 0C 50 B9 ?? ?? ?? ?? E8");
  const uintptr_t a = mem::find_pattern(g_text, p);
  if (!a) { logf("ERROR: registration of '%s' not found", name); return false; }
  const uintptr_t f = mem::read<uint32_t>(a + 1);
  const uintptr_t o = mem::read<uint32_t>(a + 21);
  const mem::Range whole = mem::module_range(reinterpret_cast<HMODULE>(g_base));
  if (!g_text.contains(f) || !whole.contains(o) || g_text.contains(o)) { logf("ERROR: '%s' fn/owner look wrong", name); return false; }
  *fn = f; *owner = o; return true;
}

// The vtable slot a timer wrapper calls: it ends in 8B 11 8B 82 <slot*4> [5E] FF E0.
int wrapper_slot(const char* name, uintptr_t impl) {
  const mem::Range r{impl, impl + 0x80};
  const uintptr_t a = mem::find_pattern(r, "8B 11 8B 82 ?? ?? ?? ??");
  if (!a) { logf("ERROR: no vtable call in the %s wrapper", name); return -1; }
  return static_cast<int>(mem::read<uint32_t>(a + 4) / 4);
}

bool parse_layout() {
  const mem::Range pauser{g_pause_impl, g_pause_impl + 0x20};
  if (uintptr_t a = mem::find_pattern(pauser, "8B 49 08 E8 ?? ?? ?? ?? 8B 10 8B C8 8B 82 ?? ?? ?? ?? FF E0")) {
    g_slot_pause = static_cast<int>(mem::read<uint32_t>(a + 14) / 4);
    const uintptr_t getter = a + 8 + mem::read<int32_t>(a + 4);
    const mem::Range gr{getter, getter + 8};
    if (g_text.contains(getter) && mem::find_pattern(gr, "8B 81 ?? ?? ?? ?? C3") == getter)
      g_off_menumgr = mem::read<uint32_t>(getter + 2);
  }
  logf("slots: pausemenu=%d start=%d stop=%d reset=%d time=%d par=%d (MenuMgr at ctx+0x%X)",
       g_slot_pause, g_slot_start, g_slot_stop, g_slot_reset, g_slot_time, g_slot_par, g_off_menumgr);
  if (g_slot_pause < 0 || g_slot_start < 0 || g_slot_stop < 0 || g_slot_reset < 0 || g_slot_time < 0 || g_slot_par < 0) {
    logf("ERROR: could not parse the vtable slots out of the wrappers");
    return false;
  }
  return true;
}

bool resolve_class_vtables() {
  HMODULE exe = reinterpret_cast<HMODULE>(g_base);
  g_fsm_vts = mem::class_vtables(exe, kRttiFearScoreMgr);
  g_mm_vts = mem::class_vtables(exe, kRttiMenuMgr);
  for (const mem::VtableInfo& v : g_fsm_vts) if (v.offset == 0) g_fsm_vt0 = v.vtable;
  for (const mem::VtableInfo& v : g_mm_vts) {
    if (v.offset == 0) g_mm_vt0 = v.vtable;
    uintptr_t fn = 0;
    if (mem::read_safe(v.vtable + static_cast<uintptr_t>(g_slot_pause) * 4, &fn) && g_text.contains(fn)) {
      const mem::Range r{fn, fn + 8};
      if (mem::find_pattern(r, "80 79 ?? 00 74") == fn) { g_mm_sub_off = v.offset; g_mm_sub_vt = v.vtable; }
    }
  }
  if (!g_fsm_vt0 || !g_mm_vt0 || !g_mm_sub_vt) {
    logf("ERROR: class vtables not resolved (fsm=%p mm=%p pause-sub=%p)", reinterpret_cast<void*>(g_fsm_vt0),
         reinterpret_cast<void*>(g_mm_vt0), reinterpret_cast<void*>(g_mm_sub_vt));
    return false;
  }

  auto slot_fn = [&](int slot) -> uintptr_t {
    uintptr_t f = 0;
    return (mem::read_safe(g_fsm_vt0 + static_cast<uintptr_t>(slot) * 4, &f) && g_text.contains(f)) ? f : 0;
  };
  const uintptr_t f_time = slot_fn(g_slot_time), f_par = slot_fn(g_slot_par), f_stop = slot_fn(g_slot_stop);
  if (f_time && mem::find_pattern(mem::Range{f_time, f_time + 8}, "D9 81 ?? ?? ?? ?? E9") == f_time)
    g_off_timer = mem::read<uint32_t>(f_time + 2);
  if (f_par && mem::find_pattern(mem::Range{f_par, f_par + 8}, "8B 81 ?? ?? ?? ?? C3") == f_par)
    g_off_par = mem::read<uint32_t>(f_par + 2);
  if (f_stop && mem::find_pattern(mem::Range{f_stop, f_stop + 20}, "80 B9 ?? ?? ?? ?? 00 75 ?? 80 B9 ?? ?? ?? ?? 00 74") == f_stop) {
    g_off_locked = mem::read<uint32_t>(f_stop + 2);
    g_off_running = mem::read<uint32_t>(f_stop + 11);
  }
  // The per-frame update advances the mission timer and, next to it, a second
  // (cumulative) timer: fld dt; fadd [esi+timer]; fstp [esi+timer]; fld dt2; fadd [esi+X]; fstp [esi+X].
  if (g_off_timer) {
    mem::Pattern p = mem::parse_pattern("D9 44 24 ?? D8 86");
    for (int i = 0; i < 4; ++i) p.bytes.push_back(static_cast<int16_t>((g_off_timer >> (8 * i)) & 0xFF));
    mem::Pattern t = mem::parse_pattern("D9 9E");
    for (int i = 0; i < 4; ++i) t.bytes.push_back(static_cast<int16_t>((g_off_timer >> (8 * i)) & 0xFF));
    p.bytes.insert(p.bytes.end(), t.bytes.begin(), t.bytes.end());
    if (const uintptr_t a = mem::find_pattern(g_text, p)) {
      const uint32_t second = mem::read<uint32_t>(a + 16 + 6);
      if (second == mem::read<uint32_t>(a + 16 + 12)) g_off_cumul = second;
    }
  }
  logf("FearScoreMgr: timer +0x%X cumulative +0x%X running +0x%X locked +0x%X par +0x%X",
       g_off_timer, g_off_cumul, g_off_running, g_off_locked, g_off_par);
  if (!g_off_timer || !g_off_par || !g_off_running || !g_off_locked) {
    logf("ERROR: the score manager's timer functions do not have the expected shape");
    return false;
  }
  return true;
}

uint32_t fnv1(const char* text) {
  uint32_t h = kEmptyHash;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
    h *= 0x01000193u;
    h ^= *p;
  }
  return h;
}

// MSVC 2008 std::string: {proxy, union{char[16]; char*}, size, reserve}; inline while reserve < 16.
bool read_std_string(uintptr_t s, char* out, size_t n) {
  uint32_t size = 0, res = 0;
  out[0] = '\0';
  if (!mem::read_safe(s + 0x14, &size) || !mem::read_safe(s + 0x18, &res)) return false;
  if (size > 255 || res < 15 || res > 0x10000 || size > res) return false;
  uintptr_t p = s + 4;
  if (res >= 16 && !mem::read_safe(s + 4, &p)) return false;
  if (!mem::readable(reinterpret_cast<const void*>(p), size + 1)) return false;
  const size_t take = size < n - 1 ? size : n - 1;
  std::memcpy(out, reinterpret_cast<const void*>(p), take);
  out[take] = '\0';
  for (size_t i = 0; i < take; ++i)
    if (static_cast<unsigned char>(out[i]) < 0x20 || static_cast<unsigned char>(out[i]) > 0x7E) return false;
  return true;
}

// A PlayerProfilePC getter/setter pair (setter at `slot`, getter right after)
// for one int field; returns the field offset or 0.
uint32_t profile_field(int setter_slot) {
  uintptr_t setter = 0, getter = 0;
  if (setter_slot < 0 || !mem::read_safe(g_pp_vt0 + static_cast<uintptr_t>(setter_slot) * 4, &setter) ||
      !mem::read_safe(g_pp_vt0 + static_cast<uintptr_t>(setter_slot + 1) * 4, &getter))
    return 0;
  if (!g_text.contains(setter) || !g_text.contains(getter)) return 0;
  if (mem::find_pattern(mem::Range{setter, setter + 16}, "8B 44 24 04 89 81 ?? ?? ?? ?? C2 04 00") != setter) return 0;
  if (mem::find_pattern(mem::Range{getter, getter + 8}, "8B 81 ?? ?? ?? ?? C3") != getter) return 0;
  const uint32_t off = mem::read<uint32_t>(setter + 6);
  return off == mem::read<uint32_t>(getter + 2) && off < 0x1000 ? off : 0;
}

bool resolve_mission_layout() {
  HMODULE exe = reinterpret_cast<HMODULE>(g_base);
  g_pp_vts = mem::class_vtables(exe, kRttiProfile);
  for (const mem::VtableInfo& v : g_pp_vts) if (v.offset == 0) g_pp_vt0 = v.vtable;
  if (!g_pp_vt0) { logf("ERROR: mission detection: no PlayerProfilePC vtable"); return false; }

  // 1. The current mission: the profile slot PlayerProfileMgr::SetCurrentLevel
  //    writes. Exactly one of the manager's vtable functions carries the call.
  std::vector<mem::VtableInfo> mgr = mem::class_vtables(exe, kRttiProfileMgr);
  if (mgr.empty()) mgr = mem::class_vtables(exe, kRttiProfileMgrPC);
  const mem::Pattern set_current = mem::parse_pattern("8B 01 8B 54 24 ?? 8B 80 ?? ?? ?? ?? 52 FF D0");
  int setter_slot = -1, matches = 0;
  for (const mem::VtableInfo& v : mgr) {
    if (v.offset != 0) continue;
    for (int slot = 0; slot < 128; ++slot) {
      uintptr_t fn = 0;
      if (!mem::read_safe(v.vtable + static_cast<uintptr_t>(slot) * 4, &fn) || !g_text.contains(fn)) break;
      const uintptr_t a = mem::find_pattern(mem::Range{fn, fn + 0x100}, set_current);
      if (!a) continue;
      const uint32_t disp = mem::read<uint32_t>(a + 8);
      if (disp % 4 || disp > 0x2000) continue;
      setter_slot = static_cast<int>(disp / 4);
      ++matches;
    }
    if (matches) break;
  }
  if (matches != 1) {
    logf("ERROR: mission detection: PlayerProfileMgr::SetCurrentLevel matched %d vtable function(s), expected 1", matches);
    return false;
  }
  g_off_profile_level = profile_field(setter_slot);
  if (!g_off_profile_level) {
    logf("ERROR: mission detection: PlayerProfilePC slots %d/%d are not a field setter/getter pair", setter_slot, setter_slot + 1);
    return false;
  }
  g_slot_level_id = setter_slot + 1;

  // 2. The progression id, from the loop in SetCurrentLevelId's core (log only).
  const uintptr_t loop = mem::find_pattern(
      g_text, "8B 11 8B 82 ?? ?? ?? ?? FF D0 8B 13 50 8B 42 0C 8B CB FF D0 8B 13 8B F0 8B 42 0C 55 8B CB FF D0 3B C6");
  if (loop) {
    const uint32_t disp = mem::read<uint32_t>(loop + 4);
    if (disp % 4 == 0 && disp >= 4 && disp <= 0x2000) {
      g_slot_progress = static_cast<int>(disp / 4);
      g_off_profile_progress = profile_field(g_slot_progress - 1);
    }
  }

  // 3. The level list component: the interface vtable that holds both map walkers.
  g_ll_vts = mem::class_vtables(exe, kRttiLevelList);
  for (const mem::VtableInfo& v : g_ll_vts) if (v.offset == 0) g_ll_vt0 = v.vtable;
  if (!g_ll_vt0) { logf("ERROR: mission detection: no LevelListGlobalDataComponent vtable"); return false; }
  const mem::Pattern by_id = mem::parse_pattern(
      "51 8B 44 24 08 56 8B F1 8D 4C 24 0C 51 8D 54 24 08 52 8D 4E ?? 89 44 24 14 E8 ?? ?? ?? ?? 8B 44 24 04 3B 46 ?? "
      "5E 74 05 83 C0 ?? 75 07 83 C8 FF 59 C2 04 00 8B 40 ?? 59 C2 04 00");
  const mem::Pattern by_index = mem::parse_pattern(
      "8B 51 ?? 8B 02 56 8B 74 24 08 3B C2 74 0D 8B FF 39 70 ?? 74 37 8B 00 3B C2 75 F5 8B 51 ?? 8B 02 3B C2 74 0B "
      "39 70 ?? 74 23 8B 00 3B C2 75 F5 8B 49 ?? 8B 01 3B C1 74 0B 39 70 ?? 74 0F 8B 00 3B C1 75 F5 B8 C5 9D 1C 81 "
      "5E C2 04 00 8B 40 ?? 5E C2 04 00");
  int slot_by_id = -1, slot_by_index = -1;
  uint32_t idx_a = 0, idx_b = 0, sent_a = 0;
  for (const mem::VtableInfo& v : g_ll_vts) {
    int sid = -1, sidx = -1;
    for (int slot = 0; slot < 64; ++slot) {
      uintptr_t fn = 0;
      if (!mem::read_safe(v.vtable + static_cast<uintptr_t>(slot) * 4, &fn) || !g_text.contains(fn)) break;
      if (sid < 0 && mem::find_pattern(mem::Range{fn, fn + 64}, by_id) == fn) {
        sid = slot;
        g_off_map = mem::read<uint8_t>(fn + 20);
        sent_a = mem::read<uint8_t>(fn + 36);
        g_off_desc = mem::read<uint8_t>(fn + 42);
        idx_a = g_off_desc + mem::read<uint8_t>(fn + 54);
      }
      // Two walkers share this shape (one keyed on the progression index, one
      // on another field); the right one compares the same node field the
      // id lookup returns.
      if (sidx < 0 && sid >= 0 && mem::find_pattern(mem::Range{fn, fn + 96}, by_index) == fn &&
          mem::read<uint8_t>(fn + 18) == idx_a) {
        sidx = slot;
        g_off_sentinel[0] = mem::read<uint8_t>(fn + 2);
        g_off_sentinel[1] = mem::read<uint8_t>(fn + 29);
        g_off_sentinel[2] = mem::read<uint8_t>(fn + 49);
        idx_b = mem::read<uint8_t>(fn + 18);
        g_off_node_id = mem::read<uint8_t>(fn + 78);
      }
    }
    if (sid >= 0 && sidx >= 0) { slot_by_id = sid; slot_by_index = sidx; g_ll_iface_off = v.offset; break; }
  }
  if (slot_by_id < 0 || slot_by_index < 0) {
    logf("ERROR: mission detection: the level-map lookups were not found in any LevelListGlobalDataComponent vtable");
    return false;
  }
  if (sent_a != g_off_sentinel[0] || idx_a != idx_b) {
    logf("ERROR: mission detection: the two level-map walkers disagree (sentinel 0x%X/0x%X index 0x%X/0x%X)", sent_a,
         g_off_sentinel[0], idx_a, idx_b);
    return false;
  }
  g_off_node_index = idx_a;
  logf("mission detection: current mission = PlayerProfilePC slot %d (profile+0x%X), progression = slot %d (profile+0x%X); "
       "level map at iface(+0x%X)+0x%X, sentinels +0x%X/+0x%X/+0x%X, node: descriptor +0x%X index +0x%X id +0x%X (lookup slots %d/%d)",
       g_slot_level_id, g_off_profile_level, g_slot_progress, g_off_profile_progress, g_ll_iface_off, g_off_map,
       g_off_sentinel[0], g_off_sentinel[1], g_off_sentinel[2], g_off_desc, g_off_node_index, g_off_node_id, slot_by_id,
       slot_by_index);
  g_mission_layout = true;
  return true;
}

bool resolve_difficulty_layout() {
  HMODULE exe = reinterpret_cast<HMODULE>(g_base);
  g_diff_vts = mem::class_vtables(exe, kRttiDifficulty);
  for (const mem::VtableInfo& v : g_diff_vts) if (v.offset == 0) g_diff_vt0 = v.vtable;
  if (!g_diff_vt0) { logf("difficulty: no DifficultyComponent vtable"); return false; }
  // Find the interface subobject whose slot 0 is "mov eax,[ecx+X]; ret" (8B 41 XX C3):
  // the difficulty int sits at that subobject + X, i.e. component + offset + X.
  for (const mem::VtableInfo& v : g_diff_vts) {
    uintptr_t fn = 0;
    if (!mem::read_safe(v.vtable, &fn) || !g_text.contains(fn)) continue;
    if (mem::find_pattern(mem::Range{fn, fn + 4}, "8B 41 ?? C3") == fn) {
      g_off_difficulty = v.offset + mem::read<uint8_t>(fn + 2);
      break;
    }
  }
  if (!g_off_difficulty) { logf("difficulty: could not find the difficulty getter"); return false; }
  logf("difficulty: DifficultyComponent difficulty int at component+0x%X", g_off_difficulty);
  g_diff_layout = true;
  return true;
}

bool is_instance(uintptr_t obj, const std::vector<mem::VtableInfo>& vts) {
  for (const mem::VtableInfo& v : vts) {
    uintptr_t vp = 0;
    if (!mem::read_safe(obj + v.offset, &vp) || vp != v.vtable) return false;
  }
  return true;
}

uintptr_t scan_for(const char* what, uintptr_t vt0, const std::vector<mem::VtableInfo>& vts) {
  size_t scanned = 0;
  std::vector<uintptr_t> hits = mem::find_objects_by_vtable(vt0, 16, &scanned);
  for (uintptr_t h : hits)
    if (is_instance(h, vts)) { logf("%s instance %p", what, reinterpret_cast<void*>(h)); return h; }
  return 0;
}

bool scan() {
  HMODULE exe = GetModuleHandleA(nullptr);
  g_base = reinterpret_cast<uintptr_t>(exe);
  g_text = mem::section(exe, ".text");
  g_data = mem::data_sections(exe);
  if (g_text.empty() || g_data.empty()) { set_status("exe sections not found"); return false; }

  struct Wrapper { const char* name; int* slot; };
  Wrapper wrappers[] = {{"StartMissionTimer", &g_slot_start}, {"StopMissionTimer", &g_slot_stop},
                        {"ResetMissionTimer", &g_slot_reset}, {"GetMissionTime", &g_slot_time},
                        {"GetParTime", &g_slot_par}};
  uintptr_t owner = 0;
  for (Wrapper& w : wrappers) {
    uintptr_t impl = 0, site = 0, o = 0;
    if (!find_a(w.name, &impl, &site) || !find_owner_a(site, &o)) return false;
    if (owner && o != owner) { logf("ERROR: %s has a different owner", w.name); return false; }
    owner = o;
    *w.slot = wrapper_slot(w.name, impl);
  }
  uintptr_t level = 0, game_owner = 0, game_owner2 = 0;
  if (!find_b("IsPauseMenuShowing", &g_pause_impl, &game_owner)) return false;
  if (!find_b("HasPlayerStartedLevel", &level, &game_owner2)) return false;
  if (game_owner != game_owner2) { logf("ERROR: the two Game methods have different owners"); return false; }
  g_fearscore_static = owner;
  g_game_static = game_owner;
  g_level = reinterpret_cast<LevelFn>(level);
  logf("class descriptors: FEARScore@exe+0x%X Game@exe+0x%X", rva(owner), rva(game_owner));
  if (!parse_layout() || !resolve_class_vtables()) return false;
  if (!resolve_mission_layout()) logf("mission detection unavailable - the target time falls back to the game's own par");
  if (!resolve_difficulty_layout()) logf("difficulty detection unavailable");
  return true;
}

bool identity_ok(uintptr_t obj, const char* expected, bool* pending) {
  uintptr_t p = 0;
  *pending = false;
  if (!mem::read_safe(obj + 0xC, &p)) return false;
  if (!p) { *pending = true; return false; }
  const size_t n = std::strlen(expected) + 1;
  if (!mem::readable(reinterpret_cast<void*>(p), n)) return false;
  return std::memcmp(reinterpret_cast<const void*>(p), expected, n) == 0;
}

bool verify_identity() {
  const DWORD start = GetTickCount();
  for (;;) {
    bool pa = false, pb = false;
    const bool a = identity_ok(g_fearscore_static, "GameScriptFEARScore", &pa);
    const bool b = identity_ok(g_game_static, "GameScriptGame", &pb);
    if (a && b) return true;
    if (!pa && !pb) { set_status("script classes are not what the scan expected"); return false; }
    if (GetTickCount() - start > 60000) { set_status("script classes never constructed"); return false; }
    Sleep(50);
  }
}

uintptr_t score_mgr_obj() {
  const uintptr_t o = g_fsm_obj;
  if (!o) return 0;
  uintptr_t vp = 0;
  if (!mem::read_safe(o, &vp) || vp != g_fsm_vt0) { g_fsm_obj = 0; return 0; }
  return o;
}

uintptr_t menu_mgr_obj() {
  const uintptr_t o = g_mm_obj;
  if (!o) return 0;
  uintptr_t vp = 0;
  if (!mem::read_safe(o, &vp) || vp != g_mm_vt0) { g_mm_obj = 0; return 0; }
  return o;
}

uintptr_t level_list_obj() {
  const uintptr_t o = g_ll_obj;
  if (!o) return 0;
  uintptr_t vp = 0;
  if (!mem::read_safe(o, &vp) || vp != g_ll_vt0) { g_ll_obj = 0; return 0; }
  return o;
}

int profile_objs(uintptr_t* out, int max) {
  int n = 0;
  const LONG count = g_pp_count;
  for (LONG i = 0; i < count && n < max; ++i) {
    uintptr_t vp = 0;
    if (mem::read_safe(g_pp_objs[i], &vp) && vp == g_pp_vt0) out[n++] = g_pp_objs[i];
  }
  if (n == 0 && count > 0) g_pp_count = 0;  // all stale: let the mod thread scan again
  return n;
}

struct LevelRecord {
  uintptr_t node = 0;
  int index = -1;
  uint32_t id = 0;
  char name[64] = {};
  bool name_ok = false;
};

// Walk one of the component's node lists (0 = the levelMap).
int level_records(int which, LevelRecord* out, int max) {
  const uintptr_t comp = level_list_obj();
  if (!comp || !g_mission_layout || which < 0 || which > 2) return 0;
  uintptr_t sentinel = 0, node = 0;
  if (!mem::read_safe(comp + g_ll_iface_off + g_off_sentinel[which], &sentinel) || !sentinel) return 0;
  if (!mem::read_safe(sentinel, &node)) return 0;
  int n = 0;
  for (int guard = 0; node && node != sentinel && guard < 256 && n < max; ++guard) {
    LevelRecord& r = out[n];
    r.node = node;
    if (!mem::read_safe(node + g_off_node_index, &r.index) || !mem::read_safe(node + g_off_node_id, &r.id)) break;
    r.name_ok = read_std_string(node + g_off_desc + g_off_desc_name, r.name, sizeof(r.name));
    ++n;
    if (!mem::read_safe(node, &node)) break;
  }
  return n;
}

}  // namespace

bool discover() {
  set_status("scanning");
  if (!scan()) { if (!std::strcmp(g_status, "scanning")) set_status("signature scan failed - see log"); return false; }
  if (!verify_identity()) return false;
  g_ready = true;
  set_status("ready");
  logf("game layer ready - looking for the score and menu managers");
  return true;
}

void instance_loop() {
  if (!g_ready) return;
  for (;;) {
    if (!g_fsm_obj) g_fsm_obj = scan_for("FearScoreMgr", g_fsm_vt0, g_fsm_vts);
    if (!g_mm_obj) g_mm_obj = scan_for("MenuMgr", g_mm_vt0, g_mm_vts);
    if (g_mission_layout) {
      if (!g_ll_obj) g_ll_obj = scan_for("LevelListGlobalDataComponent", g_ll_vt0, g_ll_vts);
      if (!g_diff_obj && g_diff_layout) g_diff_obj = scan_for("DifficultyComponent", g_diff_vt0, g_diff_vts);
      if (g_pp_count == 0) {
        size_t scanned = 0;
        std::vector<uintptr_t> hits = mem::find_objects_by_vtable(g_pp_vt0, 32, &scanned);
        int n = 0;
        for (uintptr_t h : hits) {
          if (n >= kMaxProfiles || !is_instance(h, g_pp_vts)) continue;
          g_pp_objs[n++] = h;
          logf("PlayerProfilePC instance %p", reinterpret_cast<void*>(h));
        }
        g_pp_count = n;
      }
    }
    Sleep(2000);
  }
}

bool ready() { return g_ready; }
bool context_ready() { return g_ready && g_fsm_obj && g_mm_obj; }

bool pause_menu_showing() {
  const uintptr_t m = menu_mgr_obj();
  if (!m) return false;
  const uintptr_t sub = m + g_mm_sub_off;
  uintptr_t vp = 0;
  if (!mem::read_safe(sub, &vp) || vp != g_mm_sub_vt) return false;
  auto fn = mem::read<ThisCall0B>(g_mm_sub_vt + static_cast<uintptr_t>(g_slot_pause) * 4);
  return fn(reinterpret_cast<void*>(sub), nullptr);
}

bool level_started(int player) {
  if (!g_ready || player < 0 || player >= 16) return false;
  return g_level(player);
}

float mission_time_f() {
  const uintptr_t m = score_mgr_obj();
  float v = 0.0f;
  if (!m || !mem::read_safe(m + g_off_timer, &v)) return -1.0f;
  return v;
}
int mission_time() { const float v = mission_time_f(); return v < 0.0f ? -1 : static_cast<int>(v); }

float cumulative_time_f() {
  const uintptr_t m = score_mgr_obj();
  float v = 0.0f;
  if (!m || !g_off_cumul || !mem::read_safe(m + g_off_cumul, &v)) return -1.0f;
  return v;
}

int par_time() {
  const uintptr_t m = score_mgr_obj();
  int v = 0;
  if (!m || !mem::read_safe(m + g_off_par, &v)) return -1;
  return v;
}

bool timer_running() {
  const uintptr_t m = score_mgr_obj();
  uint8_t v = 0;
  return m && mem::read_safe(m + g_off_running, &v) && v != 0;
}
bool timer_locked() {
  const uintptr_t m = score_mgr_obj();
  uint8_t v = 0;
  return m && mem::read_safe(m + g_off_locked, &v) && v != 0;
}

void set_mission_time(float seconds) {
  const uintptr_t m = score_mgr_obj();
  if (!m) return;
  const float v = seconds < 0.0f ? 0.0f : seconds;
  mem::write<float>(m + g_off_timer, v);
  if (g_off_cumul) mem::write<float>(m + g_off_cumul, v);
}

float hold_time(float mission, float cumulative) {
  const uintptr_t m = score_mgr_obj();
  if (!m) return -1.0f;
  float was = -1.0f, cur = 0.0f;
  if (mem::read_safe(m + g_off_timer, &cur)) {
    was = cur;
    if (cur != mission) mem::write<float>(m + g_off_timer, mission);
  }
  if (g_off_cumul && cumulative >= 0.0f && mem::read_safe(m + g_off_cumul, &cur) && cur != cumulative)
    mem::write<float>(m + g_off_cumul, cumulative);
  return was;
}

uintptr_t difficulty_obj() {
  const uintptr_t o = g_diff_obj;
  if (!o) return 0;
  uintptr_t vp = 0;
  if (!mem::read_safe(o, &vp) || vp != g_diff_vt0) { g_diff_obj = 0; return 0; }
  return o;
}

int difficulty() {
  const uintptr_t o = difficulty_obj();
  int v = -1;
  if (!o || !g_off_difficulty || !mem::read_safe(o + g_off_difficulty, &v)) return -1;
  return (v >= 0 && v <= 3) ? v : -1;
}
bool difficulty_ready() { return g_diff_layout && difficulty_obj() != 0; }

bool mission_detection_ready() { return g_mission_layout && g_pp_count > 0 && level_list_obj() != 0; }

MissionInfo current_mission() {
  MissionInfo m;
  if (!g_mission_layout) return m;
  uintptr_t profiles[kMaxProfiles];
  const int np = profile_objs(profiles, kMaxProfiles);
  m.profile_found = np > 0;
  m.table_found = level_list_obj() != 0;
  // The first profile with a real level id wins (single player has one profile).
  for (int i = 0; i < np; ++i) {
    uint32_t id = 0;
    if (!mem::read_safe(profiles[i] + g_off_profile_level, &id)) continue;
    if (id != 0 && id != kEmptyHash) { m.level_id = id; break; }
    if (!m.level_id) m.level_id = id;
  }
  if (!m.table_found || !m.level_id || m.level_id == kEmptyHash) return m;
  LevelRecord recs[64];
  for (int which = 0; which < 3 && !m.resolved; ++which) {
    const int n = level_records(which, recs, 64);
    for (int i = 0; i < n; ++i)
      if (recs[i].id == m.level_id) {
        m.resolved = true;
        m.index = recs[i].index;
        std::snprintf(m.name, sizeof(m.name), "%s", recs[i].name_ok ? recs[i].name : "?");
        break;
      }
  }
  return m;
}

void dump_diagnostics(const char* why) {
  logf("---- diagnostics (%s) ----", why);
  if (!g_mission_layout) { logf("  mission layout not resolved"); return; }
  uintptr_t profiles[kMaxProfiles];
  const int np = profile_objs(profiles, kMaxProfiles);
  logf("  %d PlayerProfilePC object(s)", np);
  for (int i = 0; i < np; ++i) {
    uint32_t id = 0, progress = 0;
    mem::read_safe(profiles[i] + g_off_profile_level, &id);
    if (g_off_profile_progress) mem::read_safe(profiles[i] + g_off_profile_progress, &progress);
    char hex[3 * 64 + 1] = {};
    size_t pos = 0;
    for (uint32_t off = g_off_profile_level >= 0x20 ? g_off_profile_level - 0x20 : 0; off < g_off_profile_level + 0x20 && pos + 4 < sizeof(hex); off += 4) {
      uint32_t v = 0;
      mem::read_safe(profiles[i] + off, &v);
      pos += static_cast<size_t>(std::snprintf(hex + pos, sizeof(hex) - pos, "%08X ", v));
    }
    logf("  profile %p: current mission id 0x%08X, progression id 0x%08X  dwords from +0x%X: %s",
         reinterpret_cast<void*>(profiles[i]), id, progress, g_off_profile_level >= 0x20 ? g_off_profile_level - 0x20 : 0, hex);
  }
  const uintptr_t comp = level_list_obj();
  logf("  LevelListGlobalDataComponent %p", reinterpret_cast<void*>(comp));
  for (int which = 0; which < 3; ++which) {
    LevelRecord recs[64];
    const int n = level_records(which, recs, 64);
    logf("  list %d: %d record(s)", which, n);
    for (int i = 0; i < n; ++i) {
      const LevelRecord& r = recs[i];
      int text_id = 0, target_score = 0, interval_text = 0;
      mem::read_safe(r.node + g_off_desc + 0x4, &text_id);
      mem::read_safe(r.node + g_off_desc + 0x100, &interval_text);
      mem::read_safe(r.node + g_off_desc + 0x104, &target_score);
      const uint32_t h = r.name_ok ? fnv1(r.name) : 0;
      logf("    index %2d id 0x%08X name \"%s\"%s textId %d intervalNameTextId %d targetScore %d (node %p)", r.index, r.id,
           r.name_ok ? r.name : "?", r.name_ok ? (h == r.id ? " [id = FNV-1(name)]" : " [id != FNV-1(name)]") : " [name unreadable]",
           text_id, interval_text, target_score, reinterpret_cast<void*>(r.node));
    }
  }
  const uintptr_t sm = score_mgr_obj();
  if (sm) {
    float t = 0, c = 0; int par = 0; uint8_t running = 0, locked = 0;
    mem::read_safe(sm + g_off_timer, &t); mem::read_safe(sm + g_off_cumul, &c); mem::read_safe(sm + g_off_par, &par);
    mem::read_safe(sm + g_off_running, &running); mem::read_safe(sm + g_off_locked, &locked);
    logf("  FearScoreMgr %p: time %.2f cumulative %.2f par %d running %d locked %d", reinterpret_cast<void*>(sm), t, c, par, running, locked);
  }
  logf("---- end diagnostics ----");
}

uintptr_t exe_base() { return g_base; }
const char* status_text() { return g_status; }

}  // namespace f3tm::game
