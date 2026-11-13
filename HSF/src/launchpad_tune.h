// The "Tune LaunchPad for FG" button's choices, kept apart from ReShade so a test can check them. HSF only sets LaunchPad's own settings through
// ReShade, as a user would in ReShade's menu: Marty's file is never changed or shipped.
#pragma once

#include <cstddef>
#include <vector>

namespace hsf::launchpad {

// Flow Quality: 0 Low, 1 Medium, 2 High. High searches three times as many steps at full size and gives up later.
constexpr int BEST_QUALITY = 2;

// The menu's LAUNCHPAD row (HSF.ini LaunchPadQuality): -1 nothing picked (LaunchPad keeps the player's own quality), 0 to 2 that Flow Quality,
// 3 AUTO (HSF picks the quality from the game's resolution, and picks again when it changes).
constexpr int MODE_NONE = -1;
constexpr int MODE_AUTO = 3;

// AUTO's pick. LaunchPad works the flow out at the game's full size, so its cost grows with the pixel count: the more pixels, the fewer search
// steps, to keep the cost about the same. Up to 1920x1200 High, up to 3440x1440 (ultrawide) Medium, above that (4K) Low.
inline int AutoQuality(unsigned width, unsigned height) {
  const unsigned long long pixels = static_cast<unsigned long long>(width) * height;
  if (pixels == 0) return 1;
  if (pixels <= 1920ull * 1200) return 2;
  if (pixels <= 3440ull * 1440) return 1;
  return 0;
}

// The Flow Quality a mode asks for, or -1 for none.
inline int ModeQuality(int mode, unsigned width, unsigned height) {
  if (mode == MODE_AUTO) return AutoQuality(width, height);
  return mode >= 0 && mode <= 2 ? mode : -1;
}

// ReShade's defaults (ReShade.fxh and Marty's mmx_depth.fxh agree) when a depth setting is not defined anywhere.
constexpr bool DEFAULT_REVERSED = true;
constexpr bool DEFAULT_UPSIDE_DOWN = false;

// One loaded technique, in ReShade's render order.
struct Technique {
  bool launchpad = false;  // MartysMods_Launchpad
  bool hsf = false;        // one of HSF.fx's: they only copy depth and leave the picture alone, so they may come first
  bool enabled = false;
};

// Where LaunchPad sits, or -1 if it is not loaded.
inline int LaunchPadAt(const std::vector<Technique>& order) {
  for (size_t i = 0; i < order.size(); ++i)
    if (order[i].launchpad) return static_cast<int>(i);
  return -1;
}

// True if an effect that is switched on runs before LaunchPad, so it would read last frame's flow, normals and albedo. Marty asks for LaunchPad at the top.
inline bool NeedsMove(const std::vector<Technique>& order) {
  const int at = LaunchPadAt(order);
  if (at < 0) return false;
  for (int i = 0; i < at; ++i)
    if (order[i].enabled && !order[i].hsf) return true;
  return false;
}

// The new order as indexes into `order`: LaunchPad first, the rest as they were.
inline std::vector<size_t> LaunchPadFirst(const std::vector<Technique>& order) {
  std::vector<size_t> result;
  const int at = LaunchPadAt(order);
  if (at >= 0) result.push_back(static_cast<size_t>(at));
  for (size_t i = 0; i < order.size(); ++i)
    if (static_cast<int>(i) != at) result.push_back(i);
  return result;
}

// A depth setting's value as ReShade's preprocessor reads it ("0", "1", or anything else counted as on).
inline bool DefinitionOn(const char* value) { return value != nullptr && value[0] != '\0' && !(value[0] == '0' && value[1] == '\0'); }

}  // namespace hsf::launchpad
