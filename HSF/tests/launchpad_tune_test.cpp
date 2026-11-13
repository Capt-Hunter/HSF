// Checks the LaunchPad tune button's choices: when LaunchPad has to move to the top, and the order it gets.
#include <cstdio>

#include "../src/launchpad_tune.h"

using hsf::launchpad::Technique;

static int g_failed = 0;

static void Check(bool ok, const char* what) {
  std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_failed;
}

int main() {
  const Technique lp{true, false, true};
  const Technique hsf_on{false, true, true};
  const Technique other_on{false, false, true};
  const Technique other_off{false, false, false};

  Check(!hsf::launchpad::NeedsMove({lp, other_on, hsf_on}), "already first: stays");
  Check(!hsf::launchpad::NeedsMove({hsf_on, hsf_on, lp, other_on}), "only HSF's own before it: stays");
  Check(!hsf::launchpad::NeedsMove({other_off, lp, other_on}), "only switched-off effects before it: stays");
  Check(hsf::launchpad::NeedsMove({other_on, lp}), "a switched-on effect before it: moves");
  Check(!hsf::launchpad::NeedsMove({other_on, hsf_on}), "LaunchPad not loaded: nothing to move");

  const std::vector<size_t> order = hsf::launchpad::LaunchPadFirst({other_on, hsf_on, lp, other_off});
  Check(order == std::vector<size_t>({2, 0, 1, 3}), "LaunchPad first, the rest kept in order");
  Check(hsf::launchpad::LaunchPadFirst({other_on, hsf_on}) == std::vector<size_t>({0, 1}), "no LaunchPad: order unchanged");

  Check(hsf::launchpad::DefinitionOn("1"), "\"1\" is on");
  Check(!hsf::launchpad::DefinitionOn("0"), "\"0\" is off");
  Check(!hsf::launchpad::DefinitionOn(""), "empty is off");
  Check(hsf::launchpad::DefinitionOn("2"), "\"2\" is on");

  Check(hsf::launchpad::AutoQuality(1920, 1080) == 2, "AUTO at 1080p: High");
  Check(hsf::launchpad::AutoQuality(1920, 1200) == 2, "AUTO at 1920x1200: High");
  Check(hsf::launchpad::AutoQuality(2560, 1440) == 1, "AUTO at 1440p: Medium");
  Check(hsf::launchpad::AutoQuality(3440, 1440) == 1, "AUTO at 3440x1440: Medium");
  Check(hsf::launchpad::AutoQuality(3840, 2160) == 0, "AUTO at 4K: Low");
  Check(hsf::launchpad::AutoQuality(0, 0) == 1, "AUTO with no size yet: Medium");
  Check(hsf::launchpad::ModeQuality(hsf::launchpad::MODE_AUTO, 3840, 2160) == 0, "mode AUTO follows the size");
  Check(hsf::launchpad::ModeQuality(2, 3840, 2160) == 2, "a picked quality stays at any size");
  Check(hsf::launchpad::ModeQuality(hsf::launchpad::MODE_NONE, 1920, 1080) == -1, "nothing picked: no quality");

  std::printf(g_failed == 0 ? "all passed\n" : "%d FAILED\n", g_failed);
  return g_failed == 0 ? 0 : 1;
}
