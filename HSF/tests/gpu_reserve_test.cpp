// Checks the GPU reserve: the spike count, the governor against a simple card model, the load counter's sum, and one live reading from Windows.
#include <cstdio>

#include "../src/gpu_reserve.h"

using hsf::reserve::Governor;
using hsf::reserve::Input;
using hsf::reserve::LoadReader;

static int g_failed = 0;

static void Check(bool ok, const char* what) {
  std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_failed;
}

// A card that could show `most` frames a second at 100% load. Below that the load follows the frame rate. `starves`: within 3% of its limit
// the game's frames stutter (frame generation crowds them out); a card that does not starve stays smooth even at 100%.
struct Card {
  float most = 160;
  bool starves = true;
  float cpu_spikes = 0;  // stutter that is not the card's doing (shader compiles, streaming)
  float Fps(uint32_t cap) const { return cap == 0 || cap > most ? most : static_cast<float>(cap); }
  float Load(uint32_t cap) const { return cap == 0 || cap >= most ? 100.f : 100.f * static_cast<float>(cap) / most; }
  float Spikes(uint32_t cap) const { return starves && Fps(cap) >= most * 0.97f ? 0.10f : cpu_spikes; }
};

// Runs the governor for `seconds`, one reading a second; returns the cap in force at the end.
static uint32_t Run(Governor& g, const Card& card, double& clock, int seconds, uint32_t multiplier, uint32_t user_cap, bool generating = true,
                    bool hold = false, bool on = true) {
  for (int i = 0; i < seconds; ++i) {
    clock += 1;
    const uint32_t cap = g.InForce(user_cap);
    Input in;
    in.seconds = clock;
    in.load = card.Load(cap);
    in.spikes = card.Spikes(cap);
    in.generating = generating;
    in.hold = hold;
    in.out_fps = card.Fps(cap);
    in.multiplier = multiplier;
    in.user_cap = user_cap;
    in.on = on;
    g.Update(in);
  }
  return g.InForce(user_cap);
}

int main() {
  {
    std::vector<float> even(60, 16.7f);
    Check(hsf::reserve::Spikes(even.data(), even.size()) == 0.f, "spikes: even frames have none");
    std::vector<float> some(60, 16.7f);
    some[5] = some[30] = some[50] = 40.f;
    Check(hsf::reserve::Spikes(some.data(), some.size()) == 3.f / 60.f, "spikes: three long frames out of 60");
    std::vector<float> small(60, 4.f);
    small[10] = 7.f;
    Check(hsf::reserve::Spikes(small.data(), small.size()) == 0.f, "spikes: 4 ms to 7 ms at high fps is not a spike (under 4 ms longer)");
    Check(hsf::reserve::Spikes(some.data(), 5) == 0.f, "spikes: too few frames to judge");
  }
  {
    // RE Village's case: the card at 100% with even frames. The reserve must cost nothing.
    Governor g;
    Card card;
    card.starves = false;
    double clock = 100;
    Check(Run(g, card, clock, 120, 2, 0) == 0 && g.Cap() == 0, "full card, smooth frames: no cap, no cost");
  }
  {
    Governor g;
    Card card;
    card.cpu_spikes = 0.10f;
    card.most = 400;  // the card has room
    double clock = 100;
    Check(Run(g, card, clock, 120, 2, 140) == 140 && g.Cap() == 0, "stutter with room on the card (not frame generation's doing): no cap");
  }
  {
    Governor g;
    Card card;
    double clock = 100;
    Run(g, card, clock, 4, 4, 0);
    Check(g.Cap() == 0, "settles first: no change in the first seconds");
    const uint32_t cap = Run(g, card, clock, 60, 4, 0);
    std::printf("      full card that stutters, no cap of the player's: cap %u (card makes %.0f)\n", cap, card.most);
    Check(cap != 0 && card.Spikes(cap) == 0.f, "full card that stutters: the cap comes down until the stutter stops");
    Check(cap >= 150, "and only just: a step or two under the card's limit");
    const uint32_t settled = Run(g, card, clock, 600, 4, 0);
    std::printf("      after 600 s more: cap %u\n", settled);
    Check(settled != 0 && settled < card.most * 0.97f + 1, "keeps the game out of stutter: tries a step up, backs off, waits longer each time");
  }
  {
    Governor g;
    Card card;
    double clock = 100;
    const uint32_t cap = Run(g, card, clock, 60, 2, 160);
    Check(cap < 160 && card.Spikes(cap) == 0.f, "player's cap at the card's limit and stuttering: the reserve goes under it");
    Check(Run(g, card, clock, 30, 2, 120) == 120 && g.Cap() == 0, "player lowers the cap under the reserve's: the player's holds, the reserve lets go");
  }
  {
    // Weak card at 6x: 150 on screen is 25 a second for the game, under the floor (30 x 6 = 180).
    Governor g;
    Card card;
    card.most = 150;
    double clock = 100;
    const uint32_t cap = Run(g, card, clock, 60, 6, 0);
    Check(cap >= 180 && g.Full(), "still stuttering with the game at 30: GPU FULL, never under 30 for the game");
  }
  {
    Governor g;
    Card card;
    double clock = 100;
    Run(g, card, clock, 60, 4, 0);
    const uint32_t held = g.Cap();
    Check(held != 0, "acting before the menu opens");
    Check(Run(g, card, clock, 30, 4, 0, true, true) == held, "menu open: the cap is held as it is");
    Run(g, card, clock, 1, 4, 0, false);
    Check(g.Cap() == held, "one reading with generation off (a short gap): the cap stays");
    Run(g, card, clock, 2, 4, 0, false);
    Check(g.Cap() == 0, "generation off: the reserve lets go");
    Run(g, card, clock, 60, 4, 0);
    Check(g.Cap() != 0, "generation on again: acts again");
    Run(g, card, clock, 2, 4, 0, true, false, false);
    Check(g.Cap() == 0, "reserve OFF: lets go at once");
  }
  {
    // The 17 fps lock seen in RE Village: a cap found at 2x kept after a switch to 6x.
    Governor g;
    Card card;
    double clock = 100;
    Run(g, card, clock, 60, 2, 0);
    const uint32_t at_2x = g.Cap();
    Run(g, card, clock, 1, 6, 0);
    Check(at_2x != 0 && g.Cap() == 0, "multiplier changed: the cap found at 2x is let go");
    Run(g, card, clock, 60, 6, 0);
    Check(g.Cap() == 0 || g.Cap() / 6 >= 26, "then judged again at 6x from the frame rate there");
  }
  {
    // A quieter scene: the stutter is gone and the card has room, the cap goes back up and lets go.
    Governor g;
    Card card;
    double clock = 100;
    Run(g, card, clock, 60, 2, 0);
    Check(g.Cap() != 0, "heavy scene: acting");
    card.most = 400;
    Run(g, card, clock, 900, 2, 0);
    std::printf("      quiet scene after 900 s: cap %u\n", g.Cap());
    Check(g.Cap() == 0 || g.Cap() > 300, "quiet scene: the frames come back");
  }

  Check(LoadReader::Busiest({{L"pid_1_luid_0x0_0x1_phys_0_eng_0_engtype_3D", 40}, {L"pid_2_luid_0x0_0x1_phys_0_eng_0_engtype_3D", 35},
                             {L"pid_2_luid_0x0_0x1_phys_0_eng_1_engtype_Compute", 50}}) == 75.f,
        "load: per engine summed over processes, the busiest engine");
  Check(LoadReader::Busiest({{L"pid_1_luid_0x0_0x1_phys_0_eng_0_engtype_3D", 70}, {L"pid_1_luid_0x0_0x2_phys_0_eng_0_engtype_3D", 20}}) == 70.f,
        "load: two cards are not added together");
  Check(LoadReader::Busiest({{L"pid_1_luid_0x0_0x1_phys_0_eng_0_engtype_3D", 80}, {L"pid_2_luid_0x0_0x1_phys_0_eng_0_engtype_3D", 70}}) == 100.f,
        "load: never over 100%");

  {
    auto* reader = new LoadReader;  // kept: its thread uses it until the process ends
    reader->Want(true);
    uint32_t serial = 0;
    float load = -1;
    for (int i = 0; i < 40 && serial == 0; ++i) {
      Sleep(100);
      load = reader->Load(&serial);
    }
    std::printf("      live card load: %.1f%% (reading %u)\n", load, serial);
    Check(!reader->Failed() && serial != 0 && load >= 0.f && load <= 100.f, "live: Windows' GPU counters give a reading within 4 s");
  }

  std::printf(g_failed == 0 ? "all passed\n" : "%d failed\n", g_failed);
  return g_failed == 0 ? 0 : 1;
}
