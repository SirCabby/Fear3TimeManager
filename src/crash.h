#pragma once

namespace f3tm {

// Install a last-resort exception filter that records where a fatal fault
// happened, and in which module. Injected mods are the usual suspect for
// crashes at shutdown, so it needs to be possible to tell from the log whether
// the fault is in this DLL or somewhere else entirely.
void install_crash_logger();

// Hook MSVCR90's _purecall in the exe so an R6025 records where it came from
// instead of only showing a dialog. Diagnostic only (Trace = 1 in the ini):
// it patches an import-table slot that has to be put back on unload.
void install_purecall_logger();
void remove_purecall_logger();

}  // namespace f3tm
