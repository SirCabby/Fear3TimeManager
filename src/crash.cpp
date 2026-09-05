#include "crash.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "log.h"
#include "mem.h"

namespace f3tm {
namespace {

LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;

const char* code_name(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
    default: return "exception";
  }
}

void describe(void* addr, char* out, size_t n) {
  HMODULE owner = nullptr;
  char path[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCSTR>(addr), &owner) &&
      owner) {
    GetModuleFileNameA(owner, path, MAX_PATH);
    const char* leaf = std::strrchr(path, '\\');
    std::snprintf(out, n, "%s+0x%X", leaf ? leaf + 1 : path,
                  static_cast<unsigned>(reinterpret_cast<uintptr_t>(addr) -
                                        reinterpret_cast<uintptr_t>(owner)));
  } else {
    std::snprintf(out, n, "%p (no module)", addr);
  }
}

LONG WINAPI on_exception(EXCEPTION_POINTERS* info) {
  if (info && info->ExceptionRecord) {
    void* at = info->ExceptionRecord->ExceptionAddress;
    char where[MAX_PATH + 32];
    describe(at, where, sizeof(where));
    logf("CRASH: %s (0x%08lX) at %s (thread %lu)", code_name(info->ExceptionRecord->ExceptionCode),
         info->ExceptionRecord->ExceptionCode, where, GetCurrentThreadId());
    if (info->ContextRecord) {
      logf("       eip=%08lX esp=%08lX ebp=%08lX eax=%08lX ecx=%08lX edx=%08lX",
           info->ContextRecord->Eip, info->ContextRecord->Esp, info->ContextRecord->Ebp,
           info->ContextRecord->Eax, info->ContextRecord->Ecx, info->ContextRecord->Edx);
      // A few return addresses off the stack, for the ones that land in a module.
      auto* sp = reinterpret_cast<void**>(info->ContextRecord->Esp);
      for (int i = 0, shown = 0; i < 128 && shown < 8; ++i) {
        if (!mem::readable(sp + i, sizeof(void*))) break;
        void* v = sp[i];
        HMODULE owner = nullptr;
        if (v && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    static_cast<LPCSTR>(v), &owner) &&
            owner) {
          describe(v, where, sizeof(where));
          logf("       stack[%03d] %s", i, where);
          ++shown;
        }
      }
    }
  }
  return g_previous ? g_previous(info) : EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------
// R6025 "pure virtual function call" does not raise an SEH exception - the CRT
// calls _purecall, which prints the dialog and aborts - so the filter above
// never sees it. The exe imports _purecall from MSVCR90, so hook that import
// instead and record who called it.
using PurecallFn = void(__cdecl*)();
PurecallFn g_orig_purecall = nullptr;

void __cdecl hk_purecall() {
  void* caller = __builtin_return_address(0);
  char where[MAX_PATH + 32];
  describe(caller, where, sizeof(where));
  logf("PURECALL (R6025): pure virtual called from %s", where);

  auto* frame = reinterpret_cast<void**>(__builtin_frame_address(0));
  for (int i = 0, shown = 0; i < 64 && shown < 6; ++i) {
    void* v = frame[i];
    if (!v) continue;
    HMODULE owner = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(v), &owner) &&
        owner) {
      describe(v, where, sizeof(where));
      logf("    stack[%02d] %s", i, where);
      ++shown;
    }
  }
  if (g_orig_purecall) g_orig_purecall();
}

}  // namespace

void install_purecall_logger() {
  if (g_orig_purecall) return;
  void* prev = mem::iat_hook(GetModuleHandleA(nullptr), "MSVCR90.dll", "_purecall",
                             reinterpret_cast<void*>(&hk_purecall));
  if (prev) {
    g_orig_purecall = reinterpret_cast<PurecallFn>(prev);
    logf("hooked _purecall in the exe (MSVCR90.dll)");
  }
}

void remove_purecall_logger() {
  if (!g_orig_purecall) return;
  mem::iat_hook(GetModuleHandleA(nullptr), "MSVCR90.dll", "_purecall",
                reinterpret_cast<void*>(g_orig_purecall));
  g_orig_purecall = nullptr;
}

void install_crash_logger() { g_previous = SetUnhandledExceptionFilter(on_exception); }

}  // namespace f3tm
