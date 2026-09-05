#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <vector>

// Small memory helpers, in the spirit of Fear2AwardUnlocker's mem.h: everything
// the mod pokes at lives in another module's pages, so every write has to
// unprotect, write, restore and flush - and every read of game memory has to be
// guarded, because a wrong guess must produce a log line, not a crash.
namespace f3tm::mem {

struct Range {
  uintptr_t begin = 0;
  uintptr_t end = 0;
  bool empty() const { return end <= begin; }
  size_t size() const { return empty() ? 0 : end - begin; }
  bool contains(uintptr_t a) const { return a >= begin && a < end; }
};

template <typename T>
inline bool write(uintptr_t target, const T& value) {
  DWORD old = 0;
  if (!VirtualProtect(reinterpret_cast<void*>(target), sizeof(T), PAGE_EXECUTE_READWRITE, &old))
    return false;
  std::memcpy(reinterpret_cast<void*>(target), &value, sizeof(T));
  VirtualProtect(reinterpret_cast<void*>(target), sizeof(T), old, &old);
  FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target), sizeof(T));
  return true;
}

template <typename T>
inline T read(uintptr_t target) {
  T v{};
  std::memcpy(&v, reinterpret_cast<const void*>(target), sizeof(T));
  return v;
}

// True when [p, p+n) is committed, readable memory. VirtualQuery-based, so it
// is safe to call on any value that merely looks like a pointer.
inline bool readable(const void* p, size_t n) {
  auto a = reinterpret_cast<uintptr_t>(p);
  if (!a) return false;
  while (true) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                     PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & ok)) return false;
    const uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    const size_t avail = region_end - a;
    if (avail >= n) return true;
    n -= avail;
    a = region_end;
  }
}

template <typename T>
inline bool read_safe(uintptr_t target, T* out) {
  if (!readable(reinterpret_cast<const void*>(target), sizeof(T))) return false;
  std::memcpy(out, reinterpret_cast<const void*>(target), sizeof(T));
  return true;
}

// Replace one entry of a vtable and hand back what was there.
inline void* hook_vtable(uintptr_t vtable, int slot, void* replacement) {
  uintptr_t entry = vtable + static_cast<uintptr_t>(slot) * sizeof(void*);
  void* original = read<void*>(entry);
  if (!write<void*>(entry, replacement)) return nullptr;
  return original;
}

// Replace one entry in a module's import address table; returns what was there.
inline void* iat_hook(HMODULE module, const char* dll_name, const char* fn_name,
                      void* replacement) {
  auto base = reinterpret_cast<uintptr_t>(module);
  auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  if (!module || dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
  auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
  auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return nullptr;

  for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
       imp->Name; ++imp) {
    const char* name = reinterpret_cast<const char*>(base + imp->Name);
    if (dll_name && _stricmp(name, dll_name) != 0) continue;
    auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
    auto* orig = reinterpret_cast<IMAGE_THUNK_DATA*>(
        base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
    for (; orig->u1.AddressOfData; ++orig, ++thunk) {
      if (IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal)) continue;
      auto* by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + orig->u1.AddressOfData);
      if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), fn_name) != 0) continue;
      void* previous = reinterpret_cast<void*>(thunk->u1.Function);
      if (!write<void*>(reinterpret_cast<uintptr_t>(&thunk->u1.Function), replacement))
        return nullptr;
      return previous;
    }
  }
  return nullptr;
}

// Redirect a relative CALL (E8 rel32) at `site` to `replacement`, returning the
// address it used to reach.
inline void* hook_call_site(uintptr_t site, void* replacement) {
  if (read<uint8_t>(site) != 0xE8) return nullptr;
  const int32_t old_rel = read<int32_t>(site + 1);
  void* previous = reinterpret_cast<void*>(site + 5 + old_rel);
  const int32_t new_rel =
      static_cast<int32_t>(reinterpret_cast<uintptr_t>(replacement) - (site + 5));
  if (!write<int32_t>(site + 1, new_rel)) return nullptr;
  return previous;
}

// --- PE sections -------------------------------------------------------------
inline IMAGE_NT_HEADERS* nt_headers(HMODULE m) {
  auto base = reinterpret_cast<uintptr_t>(m);
  auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  if (!m || dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
  auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  return nt->Signature == IMAGE_NT_SIGNATURE ? nt : nullptr;
}

inline Range section_range(HMODULE m, const IMAGE_SECTION_HEADER& s) {
  auto base = reinterpret_cast<uintptr_t>(m);
  DWORD size = s.Misc.VirtualSize ? s.Misc.VirtualSize : s.SizeOfRawData;
  return Range{base + s.VirtualAddress, base + s.VirtualAddress + size};
}

// The section with this (up to 8 character) name, or an empty range.
inline Range section(HMODULE m, const char* name) {
  auto nt = nt_headers(m);
  if (!nt) return {};
  auto* s = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    if (std::strncmp(reinterpret_cast<const char*>(s[i].Name), name, 8) == 0)
      return section_range(m, s[i]);
  return {};
}

// Every readable, non-executable, initialised section (where strings live).
inline std::vector<Range> data_sections(HMODULE m) {
  std::vector<Range> out;
  auto nt = nt_headers(m);
  if (!nt) return out;
  auto* s = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
    const DWORD c = s[i].Characteristics;
    if (!(c & IMAGE_SCN_MEM_READ) || (c & IMAGE_SCN_MEM_EXECUTE)) continue;
    if (!s[i].SizeOfRawData) continue;  // pure .bss holds no strings
    out.push_back(section_range(m, s[i]));
  }
  return out;
}

inline Range module_range(HMODULE m) {
  auto nt = nt_headers(m);
  if (!nt) return {};
  auto base = reinterpret_cast<uintptr_t>(m);
  return Range{base, base + nt->OptionalHeader.SizeOfImage};
}

// --- pattern scanning --------------------------------------------------------
// Patterns are IDA/CE style: "B8 ?? ?? ?? ?? 89 47 20". A "??" is a wildcard.
struct Pattern {
  std::vector<int16_t> bytes;  // -1 = wildcard
};

inline Pattern parse_pattern(const char* s) {
  Pattern p;
  for (const char* c = s; *c;) {
    while (*c == ' ') ++c;
    if (!*c) break;
    if (c[0] == '?') {
      p.bytes.push_back(-1);
      while (*c == '?') ++c;
      continue;
    }
    auto hex = [](char h) -> int {
      if (h >= '0' && h <= '9') return h - '0';
      if (h >= 'a' && h <= 'f') return h - 'a' + 10;
      if (h >= 'A' && h <= 'F') return h - 'A' + 10;
      return -1;
    };
    const int hi = hex(c[0]), lo = c[1] ? hex(c[1]) : -1;
    if (hi < 0 || lo < 0) break;  // malformed; stop where it stops making sense
    p.bytes.push_back(static_cast<int16_t>(hi * 16 + lo));
    c += 2;
  }
  return p;
}

// First match at or after `from` (0 = the range start), or 0 when absent.
inline uintptr_t find_pattern(const Range& r, const Pattern& p, uintptr_t from = 0) {
  const size_t n = p.bytes.size();
  if (!n || r.size() < n) return 0;
  uintptr_t a = from > r.begin ? from : r.begin;
  const uint8_t first = static_cast<uint8_t>(p.bytes[0]);
  const bool first_wild = p.bytes[0] < 0;
  for (; a + n <= r.end; ++a) {
    auto* m = reinterpret_cast<const uint8_t*>(a);
    if (!first_wild && m[0] != first) continue;
    size_t i = 1;
    for (; i < n; ++i)
      if (p.bytes[i] >= 0 && m[i] != static_cast<uint8_t>(p.bytes[i])) break;
    if (i == n) return a;
  }
  return 0;
}

inline uintptr_t find_pattern(const Range& r, const char* s, uintptr_t from = 0) {
  return find_pattern(r, parse_pattern(s), from);
}

inline std::vector<uintptr_t> find_all(const Range& r, const Pattern& p, size_t limit = 64) {
  std::vector<uintptr_t> out;
  for (uintptr_t a = find_pattern(r, p); a && out.size() < limit; a = find_pattern(r, p, a + 1))
    out.push_back(a);
  return out;
}

// A NUL-terminated string that starts on a NUL boundary (so "Foo" does not
// match the tail of "GetFoo").
inline uintptr_t find_cstring(const std::vector<Range>& ranges, const char* s) {
  const size_t n = std::strlen(s) + 1;  // include the terminator
  for (const Range& r : ranges) {
    if (r.size() < n) continue;
    for (uintptr_t a = r.begin; a + n <= r.end; ++a) {
      auto* m = reinterpret_cast<const char*>(a);
      if (m[0] != s[0] || std::memcmp(m, s, n) != 0) continue;
      if (a > r.begin && m[-1] != '\0') continue;
      return a;
    }
  }
  return 0;
}

// --- finding objects ---------------------------------------------------------
inline Pattern imm32_pattern(uint32_t v) {
  Pattern p;
  for (int i = 0; i < 4; ++i) p.bytes.push_back(static_cast<int16_t>((v >> (8 * i)) & 0xFF));
  return p;
}

struct VtableInfo {
  uint32_t offset;   // of the subobject this vtable belongs to
  uintptr_t vtable;
};

// Every vtable of a class, from its mangled RTTI name (".?AVMenuMgr@Despair@@"):
// TypeDescriptor <- CompleteObjectLocator(s) <- vtable[-1]. All in the module's
// data sections, so this works before a single object exists.
inline std::vector<VtableInfo> class_vtables(HMODULE m, const char* mangled) {
  std::vector<VtableInfo> out;
  const std::vector<Range> data = data_sections(m);
  const uintptr_t str = find_cstring(data, mangled);
  if (!str) return out;
  const uintptr_t td = str - 8;
  const Pattern ptd = imm32_pattern(static_cast<uint32_t>(td));
  for (const Range& r : data) {
    for (uintptr_t a = find_pattern(r, ptd); a; a = find_pattern(r, ptd, a + 1)) {
      const uintptr_t col = a - 0xC;
      if (col < r.begin) continue;
      if (read<uint32_t>(col) != 0 || read<uint32_t>(col + 4) > 0x10000) continue;
      const Pattern pcol = imm32_pattern(static_cast<uint32_t>(col));
      for (const Range& r2 : data)
        for (uintptr_t b = find_pattern(r2, pcol); b; b = find_pattern(r2, pcol, b + 1))
          out.push_back({read<uint32_t>(col + 4), b + 4});
    }
  }
  return out;
}

// Heap objects whose first dword is `vtable`. Reads go through
// ReadProcessMemory so a page vanishing mid-scan fails a read instead of
// faulting the thread. Image regions are skipped (the RTTI there also points at
// vtables). Returns at most `limit` hits; `scanned` receives the bytes covered.
inline std::vector<uintptr_t> find_objects_by_vtable(uintptr_t vtable, size_t limit, size_t* scanned) {
  std::vector<uintptr_t> out;
  std::vector<uint8_t> buf(1 << 20);
  if (scanned) *scanned = 0;
  MEMORY_BASIC_INFORMATION mbi{};
  uintptr_t a = 0x10000;
  while (a < 0xFFFF0000u && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const uintptr_t end = base + mbi.RegionSize;
    const bool want = mbi.State == MEM_COMMIT && mbi.Type != MEM_IMAGE && !(mbi.Protect & PAGE_GUARD) &&
                      (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY));
    if (want) {
      for (uintptr_t p = base; p < end; p += buf.size()) {
        const size_t n = (end - p) < buf.size() ? static_cast<size_t>(end - p) : buf.size();
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(p), buf.data(), n, &got) || !got) continue;
        if (scanned) *scanned += got;
        const uint32_t* w = reinterpret_cast<const uint32_t*>(buf.data());
        for (size_t i = 0; i + 4 <= got; i += 4)
          if (w[i / 4] == vtable) {
            out.push_back(p + i);
            if (out.size() >= limit) return out;
          }
      }
    }
    if (end <= a) break;
    a = end;
  }
  return out;
}

// Addresses of `needle` in committed non-image RW memory (same walk and the
// same ReadProcessMemory safety as find_objects_by_vtable).
inline std::vector<uintptr_t> find_bytes_in_memory(const std::vector<uint8_t>& needle, size_t limit, size_t* scanned) {
  std::vector<uintptr_t> out;
  if (needle.empty()) return out;
  std::vector<uint8_t> buf((1 << 20) + needle.size());
  if (scanned) *scanned = 0;
  MEMORY_BASIC_INFORMATION mbi{};
  uintptr_t a = 0x10000;
  while (a < 0xFFFF0000u && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const uintptr_t end = base + mbi.RegionSize;
    const bool want = mbi.State == MEM_COMMIT && mbi.Type != MEM_IMAGE && !(mbi.Protect & PAGE_GUARD) &&
                      (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY));
    if (want) {
      for (uintptr_t p = base; p < end; p += (1 << 20)) {
        // overlap by the needle length so a match straddling two chunks is seen
        const size_t n = static_cast<size_t>((end - p) < static_cast<uintptr_t>((1 << 20) + needle.size()) ? (end - p) : (1 << 20) + needle.size());
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(p), buf.data(), n, &got) || got < needle.size()) continue;
        if (scanned) *scanned += got;
        const uint8_t first = needle[0];
        for (size_t i = 0; i + needle.size() <= got; ++i) {
          if (buf[i] != first || std::memcmp(buf.data() + i, needle.data(), needle.size()) != 0) continue;
          if (i >= (1 << 20)) break;  // belongs to the next chunk
          out.push_back(p + i);
          if (out.size() >= limit) return out;
        }
      }
    }
    if (end <= a) break;
    a = end;
  }
  return out;
}

// --- MSVC RTTI ---------------------------------------------------------------
// The mangled class name of a polymorphic object (".?AVFearScoreMgr@Despair@@"),
// or nullptr when anything on the way there is not readable. This is the
// runtime "is this really the object I think it is" check.
inline const char* rtti_name(const void* obj) {
  uintptr_t vt = 0, col = 0, td = 0;
  if (!read_safe(reinterpret_cast<uintptr_t>(obj), &vt)) return nullptr;
  if (!vt || !read_safe(vt - 4, &col)) return nullptr;
  if (!col || !read_safe(col + 0xC, &td)) return nullptr;
  if (!td) return nullptr;
  auto* name = reinterpret_cast<const char*>(td + 8);
  if (!readable(name, 4) || name[0] != '.' || name[1] != '?') return nullptr;
  for (int i = 0; i < 512; ++i) {
    if (!readable(name + i, 1)) return nullptr;
    if (!name[i]) return name;
  }
  return nullptr;
}

inline bool rtti_is(const void* obj, const char* mangled) {
  const char* n = rtti_name(obj);
  return n && std::strcmp(n, mangled) == 0;
}

// ".?AVFearScoreMgr@Despair@@" -> "FearScoreMgr@Despair@@" for log lines.
inline const char* rtti_short(const char* mangled) {
  if (!mangled) return "(no rtti)";
  return std::strlen(mangled) > 4 ? mangled + 4 : mangled;
}

}  // namespace f3tm::mem
