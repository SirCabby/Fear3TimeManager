#pragma once

namespace f3tm::proxy {

// Load fmodex_orig.dll from `dir` (absolute path), pin it, and resolve every
// export the game imports into the jump table the thunks use. False means the
// game cannot run: the caller should tell the user and refuse to start.
bool load_original(const char* dir);

}  // namespace f3tm::proxy
