// XeMFG's 3x to 6x: Intel's multi-frame model ported to non-Intel GPUs (model 16), published into libxess_fg.dll before XeFG's first
// context. Without it XeFG makes only Intel's own 2x (xmx.h).
//
// Ported from MFGAdaUnlock-RenoDx by ImDreamt (MIT), commit 8b854ff, src/addons/mfgunlock/xemfg/vendor: port/shader_port.h,
// port/model_port.h, host_unlock.cpp and runtime_payload.cpp, themselves from dashdogy's MFG Unlocker (MIT). HSF's changes: only the one
// Intel build HSF accepts (libxess_fg.dll 1.3.1.78 and libxell.dll 1.3.2.10, so fixed addresses checked byte for byte instead of the
// layout discovery), the programs read from a file the user already has instead of embedded, nothing restored at exit, and a model
// another mod already published is used as it is.
//
// The 54 shader programs are derived from Intel's and stay under Intel's terms: HSF never ships them. They are read, without loading or
// running any code, from the user's own copy of MFGAdaUnlock's add-on (renodx-mfgunlock.addon64), matched by SHA-256.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace hsf::xmx::model {

struct Result {
  uint32_t max_frames = 1;  // generated frames per real frame the published model makes: 5 (6x), or 1 (Intel's 2x)
  std::string source;       // where the programs came from, or which mod published the model
  std::string why;          // why it stays at 2x, empty when max_frames is above 1
};

// Once per process, before XeFG's first context. folder: the game's folder (next to HSF.ini). Never throws.
Result Publish(HMODULE fg, HMODULE ll, const wchar_t* folder);

// Programs handed to XeFG's decoder so far (its pipeline build asks for them at Init).
uint32_t ProgramsSupplied();

}  // namespace hsf::xmx::model
