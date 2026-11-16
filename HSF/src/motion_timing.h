// The timings that decide together which motion source DLSS-G gets (the game's DLSS data, HSF's flow, or none), kept in one place.
// sl_game.cpp uses the in-game route's ones, hsf_addon.cpp the source pick's. Values as they were when they lived in those two files.
#pragma once

#include <cstdint>

namespace hsf::motiontiming {

// hsf_addon.cpp: a change between the game's DLSS data and the flow has to last this long (SourceHold).
constexpr uint64_t SOURCE_HOLD_MS = 1000;
// hsf_addon.cpp: the game's DLSS data read through NGX counts as fresh within this many real frames and this many ms.
constexpr uint64_t NGX_FRESH_FRAMES = 3;
constexpr uint64_t NGX_FRESH_MS = 250;
// hsf_addon.cpp: after the frame feed refused the NGX data, flow for this many frames before NGX is tried again.
constexpr uint64_t NGX_REFUSED_FRAMES = 120;

// sl_game.cpp: optical motion counts as gone once TagOptical has not run for this long.
constexpr uint64_t OPTICAL_GONE_MS = 300;
// sl_game.cpp: with flow fill on, generation stops only when neither the game's data nor HSF's flow came for this long.
constexpr uint64_t FLOW_FILL_MS = 3000;
// sl_game.cpp: generation stops when no evaluate came for this long.
constexpr uint64_t EVALUATE_GONE_MS = 250;
// sl_game.cpp: after generation goes on, quick off waits this long (DriveFrame).
constexpr uint64_t ON_SETTLE_MS = 1000;

}  // namespace hsf::motiontiming
