// HSF's menu look (ui.h). Every widget here is drawn by hand on ImGui's draw list over an invisible ImGui item, so ImGui still does the clicks,
// the layout and the keyboard focus while the look is HSF's own.
#include <imgui.h>
#include <reshade.hpp>

#include "ui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <cstring>
#include <vector>

namespace hsf::ui {
namespace {

// Segments a-g as bits 0-6 (a top, b top right, c bottom right, d bottom, e bottom left, f top left, g middle).
constexpr uint8_t DIGIT_SEGMENTS[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

float Font() { return ImGui::GetFontSize(); }
// Height of one control row: ReShade's frame height, so rows line up with any ImGui control drawn next to them.
float RowHeight() { return ImGui::GetFrameHeight(); }
float Spacing() { return Font() * 0.12f; }  // between spaced capitals

// The panel's inner edges, in screen space: the panel's padding in from its border on both sides, where the rows start. (ImGui's content
// region left out the padding on the left and kept room for a scrollbar on the right, so boxes and lines sat off the rows.)
float LeftEdge() { return ImGui::GetWindowPos().x + ImGui::GetCursorStartPos().x; }
float RightEdge() { return ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetCursorStartPos().x; }

// Width of `text` in spaced capitals at `size`.
float CapsWidth(const char* text, float size) {
  ImFont* const font = ImGui::GetFont();
  const float ratio = size / Font();
  float w = 0.f;
  char one[2] = {};
  for (const char* c = text; *c != '\0'; ++c) {
    one[0] = *c;
    w += ImGui::CalcTextSize(one).x * ratio + Spacing() * ratio;
  }
  (void)font;
  return w > 0.f ? w - Spacing() * ratio : 0.f;
}

// Draws `text` in spaced capitals at `at` with no layout item; returns its width.
float DrawCaps(ImDrawList* draw, ImVec2 at, const char* text, ImU32 colour, float size) {
  ImFont* const font = ImGui::GetFont();
  const float ratio = size / Font();
  float x = at.x;
  char one[2] = {};
  for (const char* c = text; *c != '\0'; ++c) {
    one[0] = (*c >= 'a' && *c <= 'z') ? static_cast<char>(*c - 'a' + 'A') : *c;
    draw->AddText(font, size, ImVec2(x, at.y), colour, one);
    x += ImGui::CalcTextSize(one).x * ratio + Spacing() * ratio;
  }
  return x - at.x - (x > at.x ? Spacing() * ratio : 0.f);
}

// The y of the last item's row and its height, for things drawn at the right edge of that row.
void LastRow(float* y, float* h) {
  const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
  *y = min.y;
  *h = max.y - min.y;
}

int Channel(ImU32 rgba, int shift) { return static_cast<int>((rgba >> shift) & 0xFF); }

}  // namespace

namespace {
// The theme's colours as floats (panel r g b, accent r g b): what is on screen, where its fade started and where it goes.
float g_theme_now[6] = {}, g_theme_from[6] = {}, g_theme_to[6] = {};
std::chrono::steady_clock::time_point g_theme_start;
bool g_theme_set = false;

void ThemeApply() {
  const auto v = [](int i) { return static_cast<int>(std::clamp(g_theme_now[i], 0.f, 255.f) + 0.5f); };
  const int pr = v(0), pg = v(1), pb = v(2);
  // The default look's border is the panel plus (54, 62, 56), its lines plus (30, 36, 32): the same steps keep any panel colour's look.
  const auto add = [](int c, int step) { return std::min(c + step, 255); };
  colour::PANEL = IM_COL32(pr, pg, pb, 244);
  colour::BORDER = IM_COL32(add(pr, 54), add(pg, 62), add(pb, 56), 220);
  colour::LINE = IM_COL32(add(pr, 30), add(pg, 36), add(pb, 32), 220);
  colour::BRACKET = IM_COL32(v(3), v(4), v(5), 255);
}
}  // namespace

void SetTheme(uint32_t panel_rgb, uint32_t accent_rgb) {
  const float to[6] = {static_cast<float>((panel_rgb >> 16) & 0xFF),  static_cast<float>((panel_rgb >> 8) & 0xFF),  static_cast<float>(panel_rgb & 0xFF),
                       static_cast<float>((accent_rgb >> 16) & 0xFF), static_cast<float>((accent_rgb >> 8) & 0xFF), static_cast<float>(accent_rgb & 0xFF)};
  if (!g_theme_set) {
    g_theme_set = true;
    std::memcpy(g_theme_now, to, sizeof(to));
    std::memcpy(g_theme_to, to, sizeof(to));
    ThemeApply();
    return;
  }
  if (std::memcmp(g_theme_to, to, sizeof(to)) == 0) return;
  std::memcpy(g_theme_from, g_theme_now, sizeof(g_theme_now));
  std::memcpy(g_theme_to, to, sizeof(to));
  g_theme_start = std::chrono::steady_clock::now();
}

void ThemeTick() {
  if (!g_theme_set || std::memcmp(g_theme_now, g_theme_to, sizeof(g_theme_now)) == 0) return;
  const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - g_theme_start).count();
  const float t = std::clamp(ms / static_cast<float>(FADE_MS), 0.f, 1.f);
  const float eased = t * t * (3.f - 2.f * t);
  for (int i = 0; i < 6; ++i) g_theme_now[i] = t >= 1.f ? g_theme_to[i] : g_theme_from[i] + (g_theme_to[i] - g_theme_from[i]) * eased;
  ThemeApply();
}

ImU32 WithAlpha(ImU32 rgba, int alpha) {
  return (rgba & ~IM_COL32_A_MASK) | (static_cast<ImU32>(std::clamp(alpha, 0, 255)) << IM_COL32_A_SHIFT);
}

ImU32 Darker(ImU32 rgba, float scale) {
  const auto f = [&](int shift) { return static_cast<int>(static_cast<float>(Channel(rgba, shift)) * scale + 0.5f); };
  return IM_COL32(f(IM_COL32_R_SHIFT), f(IM_COL32_G_SHIFT), f(IM_COL32_B_SHIFT), Channel(rgba, IM_COL32_A_SHIFT));
}

void BeginPanel(const char* id) {
  ThemeTick();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, colour::PANEL);
  ImGui::PushStyleColor(ImGuiCol_Border, colour::BORDER);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.f);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Font() * 1.1f, Font() * 0.9f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Font() * 0.35f, Font() * 0.45f));
  ImGui::BeginChild(id, ImVec2(0.f, 0.f), ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
  // The orange corner bracket at the top left, as on the RTSS overlay it follows.
  ImDrawList* const draw = ImGui::GetWindowDrawList();
  const ImVec2 at = ImGui::GetWindowPos();
  const float arm = Font() * 1.1f, thick = std::max(2.f, Font() * 0.13f);
  draw->AddRectFilled(at, ImVec2(at.x + arm, at.y + thick), colour::BRACKET);
  draw->AddRectFilled(at, ImVec2(at.x + thick, at.y + arm), colour::BRACKET);
}

void EndPanel() {
  ImGui::EndChild();
  ImGui::PopStyleVar(4);
  ImGui::PopStyleColor(2);
}

void Rule() {
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float y = at.y + Font() * 0.2f;
  ImGui::GetWindowDrawList()->AddLine(ImVec2(LeftEdge(), y), ImVec2(RightEdge(), y), colour::LINE, 1.f);
  ImGui::Dummy(ImVec2(1.f, Font() * 0.4f));
}

namespace {
// Discord's logo, in the units of its own artwork (127.14 wide, 96.36 high): the outline's left half, from the top centre round to the bottom
// centre. The right half is its mirror. The eyes are cut out after.
constexpr float LOGO_W = 127.14f, LOGO_H = 96.36f;
constexpr float LOGO_HALF[][2] = {
    {63.57f, 6.83f}, {49.f, 6.83f},    {47.6f, 3.3f},    {45.64f, 0.f}, {32.f, 3.2f},   {19.39f, 8.09f},  {11.5f, 20.f},  {5.5f, 34.f},
    {1.8f, 49.f},    {0.3f, 64.f},     {0.54f, 80.21f},  {10.f, 87.5f}, {21.f, 93.f},   {32.71f, 96.36f}, {36.5f, 91.f},  {39.6f, 85.25f},
    {34.f, 82.9f},   {28.75f, 80.07f}, {31.41f, 78.07f}, {40.f, 81.6f}, {51.5f, 83.6f}, {63.57f, 84.1f}};
constexpr float LOGO_EYE_X = 42.45f, LOGO_EYE_Y = 53.f, LOGO_EYE_RX = 11.45f, LOGO_EYE_RY = 12.7f;

struct Logo {
  std::vector<ImVec2> outline;  // the whole outline, in the artwork's units
  std::vector<int> triangles;   // three outline indices per triangle
};

float Cross(ImVec2 o, ImVec2 a, ImVec2 b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

// The outline, and its triangles by ear clipping: ReShade's ImGui has no fill for a shape with dents, so the logo is drawn as triangles.
const Logo& LogoShape() {
  static Logo logo;
  if (!logo.outline.empty()) return logo;
  constexpr int half = static_cast<int>(std::size(LOGO_HALF));
  for (int i = 0; i < half; ++i) logo.outline.emplace_back(LOGO_HALF[i][0], LOGO_HALF[i][1]);
  for (int i = half - 2; i >= 1; --i) logo.outline.emplace_back(LOGO_W - LOGO_HALF[i][0], LOGO_HALF[i][1]);
  const std::vector<ImVec2>& p = logo.outline;
  float area = 0.f;
  for (size_t i = 0; i < p.size(); ++i) area += Cross(ImVec2(0.f, 0.f), p[i], p[(i + 1) % p.size()]);
  const float turn = area > 0.f ? 1.f : -1.f;
  std::vector<int> left(p.size());
  for (size_t i = 0; i < p.size(); ++i) left[i] = static_cast<int>(i);
  while (left.size() > 3) {
    bool clipped = false;
    for (size_t i = 0; i < left.size(); ++i) {
      const int a = left[(i + left.size() - 1) % left.size()], b = left[i], c = left[(i + 1) % left.size()];
      if (Cross(p[a], p[b], p[c]) * turn <= 0.f) continue;  // a dent, not an ear
      bool inside = false;
      for (const int o : left) {
        if (o == a || o == b || o == c) continue;
        if (Cross(p[a], p[b], p[o]) * turn >= 0.f && Cross(p[b], p[c], p[o]) * turn >= 0.f && Cross(p[c], p[a], p[o]) * turn >= 0.f) {
          inside = true;
          break;
        }
      }
      if (inside) continue;
      logo.triangles.insert(logo.triangles.end(), {a, b, c});
      left.erase(left.begin() + static_cast<std::ptrdiff_t>(i));
      clipped = true;
      break;
    }
    if (!clipped) break;
  }
  if (left.size() == 3) logo.triangles.insert(logo.triangles.end(), {left[0], left[1], left[2]});
  return logo;
}

// The logo `h` high with its top left at `at`, the eyes in `cut`.
void DiscordLogo(ImDrawList* draw, ImVec2 at, float h, ImU32 body, ImU32 cut) {
  const Logo& logo = LogoShape();
  const float s = h / LOGO_H;
  const auto map = [&](ImVec2 v) { return ImVec2(at.x + v.x * s, at.y + v.y * s); };
  // Triangles without ImGui's soft edges, which would show seams between them, then the outline once with them, for a smooth edge.
  const ImDrawListFlags flags = draw->Flags;
  draw->Flags &= ~ImDrawListFlags_AntiAliasedFill;
  for (size_t i = 0; i + 2 < logo.triangles.size(); i += 3)
    draw->AddTriangleFilled(map(logo.outline[logo.triangles[i]]), map(logo.outline[logo.triangles[i + 1]]), map(logo.outline[logo.triangles[i + 2]]), body);
  draw->Flags = flags;
  std::vector<ImVec2> edge(logo.outline.size());
  for (size_t i = 0; i < edge.size(); ++i) edge[i] = map(logo.outline[i]);
  draw->AddPolyline(edge.data(), static_cast<int>(edge.size()), body, ImDrawFlags_Closed, 1.f);
  draw->AddEllipseFilled(map(ImVec2(LOGO_EYE_X, LOGO_EYE_Y)), LOGO_EYE_RX * s, LOGO_EYE_RY * s, cut, 0.f, 16);
  draw->AddEllipseFilled(map(ImVec2(LOGO_W - LOGO_EYE_X, LOGO_EYE_Y)), LOGO_EYE_RX * s, LOGO_EYE_RY * s, cut, 0.f, 16);
}
}  // namespace

int Header(const char* left, const char* right, const Preset* presets, int count, uint32_t panel_rgb, uint32_t accent_rgb) {
  ImDrawList* const draw = ImGui::GetWindowDrawList();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float size = Font() * 0.85f;
  DrawCaps(draw, ImVec2(at.x + Font() * 0.4f, at.y), left, colour::TEXT, size);
  const float w = CapsWidth(right, size);
  DrawCaps(draw, ImVec2(RightEdge() - w, at.y), right, colour::DIM, size);
  int clicked = -1;
  if (presets != nullptr && count > 0) {
    // One small square per preset, its panel colour with a band of its accent along the bottom, left of `right`.
    const float box = size * 0.95f, gap = box * 0.45f;
    float x = RightEdge() - w - Font() * 1.0f - static_cast<float>(count) * (box + gap) + gap;
    const ImVec2 back = ImGui::GetCursorScreenPos();
    const auto rgb = [](uint32_t v) { return IM_COL32((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF, 255); };
    for (int i = 0; i < count; ++i, x += box + gap) {
      const Preset& p = presets[i];
      const ImVec2 min(x, at.y + (size - box) * 0.5f), max(min.x + box, min.y + box);
      ImGui::SetCursorScreenPos(min);
      char id[24];
      std::snprintf(id, sizeof(id), "##preset%d", i);
      if (ImGui::InvisibleButton(id, ImVec2(box, box))) clicked = i;
      const bool hovered = ImGui::IsItemHovered();
      if (hovered) ImGui::SetTooltip("%s", p.name);
      draw->AddRectFilled(min, max, rgb(p.panel_rgb));
      draw->AddRectFilled(ImVec2(min.x, max.y - box * 0.38f), max, rgb(p.accent_rgb));
      const bool current = p.panel_rgb == panel_rgb && p.accent_rgb == accent_rgb;
      draw->AddRect(ImVec2(min.x - 1.f, min.y - 1.f), ImVec2(max.x + 1.f, max.y + 1.f), current ? colour::TEXT : hovered ? colour::DIM : colour::FAINT, 0.f, 0,
                    current ? 1.5f : 1.f);
    }
    ImGui::SetCursorScreenPos(back);
  }
  ImGui::Dummy(ImVec2(1.f, size));
  Rule();
  return clicked;
}

bool DiscordButton(const char* tip) {
  float y = 0.f, row_h = 0.f;
  LastRow(&y, &row_h);
  const float h = std::max(row_h * 1.1f, Font() * 0.75f), w = h * LOGO_W / LOGO_H;
  const ImVec2 min(RightEdge() - w, y + (row_h - h) * 0.5f);
  const ImVec2 back = ImGui::GetCursorScreenPos();
  ImGui::SetCursorScreenPos(min);
  const bool clicked = ImGui::InvisibleButton("##discord", ImVec2(w, h));
  const bool hovered = ImGui::IsItemHovered();
  if (hovered) ImGui::SetTooltip("%s", tip);
  DiscordLogo(ImGui::GetWindowDrawList(), min, h, hovered ? IM_COL32(128, 140, 255, 255) : IM_COL32(88, 101, 242, 255), colour::PANEL);
  ImGui::SetCursorScreenPos(back);
  return clicked;
}

float Caps(const char* text, ImU32 colour, float scale) {
  const float size = Font() * scale;
  const float w = DrawCaps(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), text, colour, size);
  ImGui::Dummy(ImVec2(std::max(w, 1.f), size));
  return w;
}

void Small(const char* text, ImU32 colour) {
  ImGui::PushStyleColor(ImGuiCol_Text, colour);
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
}

void SmallWrapped(const char* text, ImU32 colour) {
  ImGui::PushStyleColor(ImGuiCol_Text, colour);
  ImGui::PushTextWrapPos(0.f);
  ImGui::TextUnformatted(text);
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
}

float SegmentText(ImDrawList* draw, ImVec2 at, float h, const char* text, ImU32 colour) {
  // Narrow and thin, closer to the condensed numbers of the HUDs this follows than a calculator's.
  const float w = h * 0.48f, t = h * 0.105f, gap = t * 0.18f, mid = h * 0.5f;
  float x = at.x;
  for (const char* c = text; *c != '\0'; ++c) {
    if (*c == '.') {
      if (draw) draw->AddRectFilled(ImVec2(x, at.y + h - t), ImVec2(x + t, at.y + h), colour);
      x += t * 2.f;
      continue;
    }
    if (*c == 'x' || *c == 'X') {
      if (draw) {
        draw->AddLine(ImVec2(x, at.y + mid - t), ImVec2(x + w * 0.8f, at.y + h), colour, t);
        draw->AddLine(ImVec2(x + w * 0.8f, at.y + mid - t), ImVec2(x, at.y + h), colour, t);
      }
      x += w * 0.8f + t * 1.5f;
      continue;
    }
    if (*c < '0' || *c > '9') {
      x += w * 0.6f;
      continue;
    }
    const uint8_t s = DIGIT_SEGMENTS[*c - '0'];
    if (draw) {
      const float y = at.y;
      auto bar = [&](bool on, float x0, float y0, float x1, float y1) {
        if (on) draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), colour);
      };
      bar(s & 0x01, x + t + gap, y, x + w - t - gap, y + t);                                // a
      bar(s & 0x02, x + w - t, y + t + gap, x + w, y + mid - gap);                          // b
      bar(s & 0x04, x + w - t, y + mid + gap, x + w, y + h - t - gap);                      // c
      bar(s & 0x08, x + t + gap, y + h - t, x + w - t - gap, y + h);                        // d
      bar(s & 0x10, x, y + mid + gap, x + t, y + h - t - gap);                              // e
      bar(s & 0x20, x, y + t + gap, x + t, y + mid - gap);                                  // f
      bar(s & 0x40, x + t + gap, y + mid - t * 0.5f, x + w - t - gap, y + mid + t * 0.5f);  // g
    }
    x += w + t * 1.3f;
  }
  return x - at.x;
}

void BigReadout(const char* number, ImU32 number_colour, const char* line1, const char* line2, const char* tag, ImU32 tag_colour) {
  ImDrawList* const draw = ImGui::GetWindowDrawList();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float h = Font() * 3.0f, small = Font() * 0.8f;
  const float w = SegmentText(draw, at, h, number, number_colour);
  const float x = at.x + w + Font() * 0.7f;
  DrawCaps(draw, ImVec2(x, at.y + h * 0.18f), line1, colour::TEXT, small);
  DrawCaps(draw, ImVec2(x, at.y + h * 0.18f + small * 1.35f), line2, colour::DIM, small);
  if (tag != nullptr && tag[0] != '\0') {
    const float tw = CapsWidth(tag, small), pad = Font() * 0.45f;
    const ImVec2 box_max(RightEdge(), at.y + h * 0.5f + small * 0.5f + pad * 0.6f);
    const ImVec2 box_min(box_max.x - tw - pad * 2.f - small, box_max.y - small - pad * 1.2f);
    draw->AddRect(box_min, box_max, tag_colour, 0.f, 0, 1.f);
    draw->AddCircleFilled(ImVec2(box_min.x + pad + small * 0.3f, (box_min.y + box_max.y) * 0.5f), small * 0.22f, tag_colour);
    DrawCaps(draw, ImVec2(box_min.x + pad + small, box_min.y + pad * 0.6f), tag, tag_colour, small);
  }
  ImGui::Dummy(ImVec2(std::max(w, 1.f), h + Font() * 0.2f));
}

// Wide enough for the longest row label ("KEEP OVERLAYS OUT") in spaced capitals, with a gap.
float ControlX() { return LeftEdge() + Font() * 14.f; }

void RowLabel(const char* text, ImU32 colour) {
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float size = Font() * 0.95f;
  DrawCaps(ImGui::GetWindowDrawList(), ImVec2(at.x, at.y + (RowHeight() - size) * 0.5f), text, colour, size);
  ImGui::Dummy(ImVec2(Font() * 13.f, RowHeight()));
  ImGui::SameLine(0.f, 0.f);
  ImGui::SetCursorScreenPos(ImVec2(ControlX(), at.y));
}

bool SettingLabel(const char* text) {
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float size = Font() * 0.8f;
  ImDrawList* const draw = ImGui::GetWindowDrawList();
  // Cut short of the control column, so a long label never runs under the control.
  draw->PushClipRect(at, ImVec2(ControlX() - Font() * 0.5f, at.y + RowHeight()), true);
  DrawCaps(draw, ImVec2(at.x, at.y + (RowHeight() - size) * 0.5f), text, colour::DIM, size);
  draw->PopClipRect();
  ImGui::Dummy(ImVec2(std::max(ControlX() - at.x - Font() * 0.5f, 1.f), RowHeight()));
  const bool hovered = ImGui::IsItemHovered();
  ImGui::SameLine(0.f, 0.f);
  ImGui::SetCursorScreenPos(ImVec2(ControlX(), at.y));
  return hovered;
}

bool Choice(const char* id, const char* const* names, int count, int* value, uint32_t disabled, const char* const* tips) {
  bool changed = false;
  ImDrawList* const draw = ImGui::GetWindowDrawList();
  const float h = RowHeight(), size = Font() * 0.8f, pad = Font() * 0.6f;
  ImGui::PushID(id);
  for (int i = 0; i < count; ++i) {
    if (i > 0) ImGui::SameLine(0.f, Font() * 0.3f);
    const bool off = (disabled >> i) & 1u;
    const float w = std::max(CapsWidth(names[i], size) + pad * 2.f, h * 1.5f);
    ImGui::PushID(i);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##box", ImVec2(w, h)) && !off && *value != i) {
      *value = i;
      changed = true;
    }
    const bool hovered = !off && ImGui::IsItemHovered();
    if (hovered && tips != nullptr && tips[i] != nullptr) ImGui::SetTooltip("%s", tips[i]);
    const bool chosen = *value == i && !off;  // a crossed-out box never looks picked, even if it holds the saved value
    ImGui::PopID();
    const ImVec2 end(at.x + w, at.y + h);
    if (chosen) draw->AddRectFilled(at, end, IM_COL32(200, 255, 220, 24));
    draw->AddRect(at, end, chosen ? colour::TEXT : hovered ? colour::DIM : colour::FAINT, 0.f, 0, 1.f);
    const ImU32 text = off ? colour::FAINT : chosen ? colour::WHITE : hovered ? colour::TEXT : colour::DIM;
    const float tw = CapsWidth(names[i], size);
    DrawCaps(draw, ImVec2(at.x + (w - tw) * 0.5f, at.y + (h - size) * 0.5f), names[i], text, size);
    if (off) draw->AddLine(ImVec2(at.x + pad * 0.6f, at.y + h * 0.5f), ImVec2(end.x - pad * 0.6f, at.y + h * 0.5f), colour::FAINT, 1.f);
  }
  ImGui::PopID();
  return changed;
}

bool Toggle(const char* id, bool* value, const char* tip) {
  static const char* const NAMES[] = {"ON", "OFF"};
  const char* const tips[] = {tip, tip};
  int index = *value ? 0 : 1;
  if (!Choice(id, NAMES, 2, &index, 0, tip != nullptr ? tips : nullptr)) return false;
  *value = index == 0;
  return true;
}

bool Button(const char* id, const char* text, ImU32 accent) {
  ImDrawList* const draw = ImGui::GetWindowDrawList();
  const float h = RowHeight(), size = Font() * 0.8f, pad = Font() * 0.7f;
  const float w = CapsWidth(text, size) + pad * 2.f;
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
  const bool hovered = ImGui::IsItemHovered();
  const ImVec2 end(at.x + w, at.y + h);
  if (hovered) draw->AddRectFilled(at, end, IM_COL32(255, 255, 255, 18));
  draw->AddRect(at, end, accent, 0.f, 0, 1.f);
  DrawCaps(draw, ImVec2(at.x + pad, at.y + (h - size) * 0.5f), text, accent, size);
  return clicked;
}

void Note(const char* text, ImU32 colour) {
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float size = Font() * 0.78f;
  ImGui::PushClipRect(ImVec2(LeftEdge(), at.y), ImVec2(RightEdge(), at.y + size * 1.4f), true);
  DrawCaps(ImGui::GetWindowDrawList(), ImVec2(LeftEdge(), at.y), text, colour, size);
  ImGui::PopClipRect();
  ImGui::Dummy(ImVec2(1.f, size));
}

bool LitButton(const char* id, const char* text, bool lit) {
  ImDrawList* const draw = ImGui::GetWindowDrawList();
  const float h = RowHeight(), size = Font() * 0.8f, pad = Font() * 0.7f;
  const float w = CapsWidth(text, size) + pad * 2.f;
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
  const bool hovered = ImGui::IsItemHovered();
  const ImVec2 end(at.x + w, at.y + h);
  if (lit) draw->AddRectFilled(at, end, IM_COL32(200, 255, 220, 24));
  draw->AddRect(at, end, lit ? colour::TEXT : hovered ? colour::DIM : colour::FAINT, 0.f, 0, 1.f);
  DrawCaps(draw, ImVec2(at.x + pad, at.y + (h - size) * 0.5f), text, lit ? colour::WHITE : hovered ? colour::TEXT : colour::DIM, size);
  return clicked;
}

void KeyChip(const char* text) {
  float y = 0.f, h = 0.f;
  LastRow(&y, &h);
  ImDrawList* const draw = ImGui::GetWindowDrawList();
  const float size = Font() * 0.8f, pad = Font() * 0.4f;
  const float w = CapsWidth(text, size) + pad * 2.f;
  const ImVec2 min(RightEdge() - w, y + (h - size) * 0.5f - pad * 0.5f), max(RightEdge(), y + (h + size) * 0.5f + pad * 0.5f);
  draw->AddRect(min, max, colour::RED, 0.f, 0, 1.f);
  DrawCaps(draw, ImVec2(min.x + pad, y + (h - size) * 0.5f), text, colour::RED, size);
}

void RightNote(const char* text, ImU32 colour) {
  float y = 0.f, h = 0.f;
  LastRow(&y, &h);
  const float size = Font() * 0.8f;
  DrawCaps(ImGui::GetWindowDrawList(), ImVec2(RightEdge() - CapsWidth(text, size), y + (h - size) * 0.5f), text, colour, size);
}

ImU32 Pulsing(ImU32 colour) {
  const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 5.0f);
  const float alpha = static_cast<float>((colour >> IM_COL32_A_SHIFT) & 0xFF) * (0.35f + 0.65f * pulse);
  return (colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha) << IM_COL32_A_SHIFT);
}

void WarningIcon(ImVec2 centre, float size, ImU32 colour) {
  ImDrawList* const draw = ImGui::GetWindowDrawList();
  const float top = centre.y - size * 0.5f, bottom = centre.y + size * 0.5f, half = size * 0.58f;
  draw->AddTriangleFilled(ImVec2(centre.x, top), ImVec2(centre.x + half, bottom), ImVec2(centre.x - half, bottom), colour);
  // The "!" in the panel's colour: a bar and a dot.
  const float w = std::max(1.f, size * 0.07f);
  draw->AddRectFilled(ImVec2(centre.x - w, top + size * 0.33f), ImVec2(centre.x + w, top + size * 0.68f), colour::PANEL);
  draw->AddRectFilled(ImVec2(centre.x - w, top + size * 0.76f), ImVec2(centre.x + w, top + size * 0.76f + w * 2.f), colour::PANEL);
}

bool Bar(const char* id, int* value, int min, int max, float width, ImU32 fill, bool* released) {
  const float h = RowHeight();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  if (width <= 0.f) width = std::max(RightEdge() - at.x - Font() * 8.f, Font() * 6.f);
  ImGui::InvisibleButton(id, ImVec2(width, h));
  bool changed = false;
  if (ImGui::IsItemActive() && max > min) {
    const float t = std::clamp((ImGui::GetMousePos().x - at.x) / width, 0.f, 1.f);
    const int v = min + static_cast<int>(t * static_cast<float>(max - min) + 0.5f);
    if (v != *value) {
      *value = v;
      changed = true;
    }
  }
  *released = ImGui::IsItemDeactivated();
  const bool hovered = ImGui::IsItemHovered() || ImGui::IsItemActive();
  ImDrawList* const draw = ImGui::GetWindowDrawList();
  const float y = at.y + h * 0.5f, thick = std::max(2.f, Font() * 0.12f);
  const float t = max > min ? std::clamp(static_cast<float>(*value - min) / static_cast<float>(max - min), 0.f, 1.f) : 0.f;
  const float x = at.x + t * width;
  draw->AddRectFilled(ImVec2(at.x, y - thick * 0.5f), ImVec2(at.x + width, y + thick * 0.5f), colour::FAINT);
  draw->AddRectFilled(ImVec2(at.x, y - thick * 0.5f), ImVec2(x, y + thick * 0.5f), fill);
  const float tick = h * (hovered ? 0.36f : 0.3f);
  draw->AddRectFilled(ImVec2(x - thick * 0.6f, y - tick), ImVec2(x + thick * 0.6f, y + tick), colour::WHITE);
  return changed;
}

void StatusBox(const char* text, const char* verdict, ImU32 verdict_colour) {
  ImDrawList* const draw = ImGui::GetWindowDrawList();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float h = RowHeight() * 1.35f, size = Font() * 0.85f, pad = Font() * 0.7f;
  const ImVec2 end(RightEdge(), at.y + h);
  draw->AddRect(ImVec2(LeftEdge(), at.y), end, colour::BORDER, 0.f, 0, 1.f);
  const float vw = CapsWidth(verdict, size);
  // Clip the left text so a long reason never runs under the verdict.
  ImGui::PushClipRect(ImVec2(LeftEdge(), at.y), ImVec2(end.x - vw - pad * 2.f, end.y), true);
  DrawCaps(draw, ImVec2(LeftEdge() + pad, at.y + (h - size) * 0.5f), text, colour::TEXT, size);
  ImGui::PopClipRect();
  DrawCaps(draw, ImVec2(end.x - pad - vw, at.y + (h - size) * 0.5f), verdict, verdict_colour, size);
  ImGui::Dummy(ImVec2(1.f, h));
}

bool AlertBox(const char* id, const char* title, const char* line, const char* button, ImU32 tint) {
  ImDrawList* const draw = ImGui::GetWindowDrawList();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float big = Font() * 1.05f, small = Font() * 0.78f, pad = Font() * 0.55f;
  const float button_text = Font() * 0.95f, button_h = button != nullptr ? Font() * 1.9f : 0.f;
  const float button_w = button != nullptr ? std::max(Font() * 7.f, CapsWidth(button, button_text) + Font() * 2.f) : 0.f;
  const float button_y = at.y + pad + big + small * 1.7f;
  const float h = button != nullptr ? button_y - at.y + button_h + pad : pad * 2.f + big + small * 1.5f;
  const float left = LeftEdge(), right = RightEdge();
  // Pulses about once a second, from dim to bright, so it can't be missed.
  const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 6.0f);
  const int r = static_cast<int>((tint >> IM_COL32_R_SHIFT) & 0xFF), g = static_cast<int>((tint >> IM_COL32_G_SHIFT) & 0xFF), b = static_cast<int>((tint >> IM_COL32_B_SHIFT) & 0xFF);
  const ImU32 fill = IM_COL32(r * 4 / 5, g * 4 / 5, b * 4 / 5, static_cast<int>(60.f + 110.f * pulse));
  const ImU32 edge = IM_COL32(r, g, b, static_cast<int>(150.f + 105.f * pulse));
  draw->AddRectFilled(ImVec2(left, at.y), ImVec2(right, at.y + h), fill);
  draw->AddRect(ImVec2(left, at.y), ImVec2(right, at.y + h), edge, 0.f, 0, 2.f);
  const float tw = CapsWidth(title, big), lw = CapsWidth(line, small);
  ImGui::PushClipRect(ImVec2(left, at.y), ImVec2(right, at.y + h), true);
  DrawCaps(draw, ImVec2(left + std::max(pad, (right - left - tw) * 0.5f), at.y + pad), title, colour::WHITE, big);
  DrawCaps(draw, ImVec2(left + std::max(pad, (right - left - lw) * 0.5f), at.y + pad + big + small * 0.4f), line, colour::WHITE, small);
  ImGui::PopClipRect();
  if (button == nullptr) {
    ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + h));
    ImGui::Dummy(ImVec2(1.f, Font() * 0.3f));
    return false;
  }
  // A big solid button, centred in the box, so it can't be mistaken for decoration.
  const ImVec2 b0(left + (right - left - button_w) * 0.5f, button_y), b1(b0.x + button_w, b0.y + button_h);
  ImGui::SetCursorScreenPos(b0);
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(button_w, button_h));
  const bool hovered = ImGui::IsItemHovered();
  draw->AddRectFilled(b0, b1, hovered ? IM_COL32(255, 255, 255, 255) : IM_COL32(235, 235, 235, 230));
  DrawCaps(draw, ImVec2(b0.x + (button_w - CapsWidth(button, button_text)) * 0.5f, b0.y + (button_h - button_text) * 0.5f), button, IM_COL32(r / 2, g / 2, b / 2, 255), button_text);
  ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + h));
  ImGui::Dummy(ImVec2(1.f, Font() * 0.3f));
  return clicked;
}

void Sparkline(const float* values, size_t count, float height) {
  ImDrawList* const draw = ImGui::GetWindowDrawList();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float left = LeftEdge(), width = RightEdge() - left;
  if (count >= 2) {
    std::vector<float> sorted(values, values + count);
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(count / 2), sorted.end());
    const float top = std::max(sorted[count / 2] * 2.5f, 1.f);
    std::vector<ImVec2> points(count);
    const float step = width / static_cast<float>(count - 1);
    for (size_t i = 0; i < count; ++i)
      points[i] = ImVec2(left + step * static_cast<float>(i), at.y + height - std::min(values[i] / top, 1.f) * height);
    draw->AddPolyline(points.data(), static_cast<int>(count), colour::DIM, ImDrawFlags_None, 1.2f);
  }
  ImGui::Dummy(ImVec2(1.f, height + Font() * 0.2f));
}

bool StackRow(const char* name, ImU32 name_colour, const char* status, ImU32 status_colour) {
  ImDrawList* const draw = ImGui::GetWindowDrawList();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float h = RowHeight(), size = Font() * 0.8f, y = at.y + (h - size) * 0.5f;
  // A long name is cut short of the status column, so the two never overlap.
  draw->PushClipRect(ImVec2(at.x, at.y), ImVec2(ControlX() - Font() * 0.5f, at.y + h), true);
  DrawCaps(draw, ImVec2(at.x, y), name, name_colour, size);
  draw->PopClipRect();
  const float r = Font() * 0.2f;
  draw->AddCircleFilled(ImVec2(ControlX() + r, at.y + h * 0.5f), r, status_colour, 12);
  DrawCaps(draw, ImVec2(ControlX() + r * 2.f + Font() * 0.5f, y), status, status_colour, size);
  ImGui::Dummy(ImVec2(std::max(RightEdge() - at.x, 1.f), h));
  return ImGui::IsItemHovered();
}

bool StackDetail(const char* text, ImU32 colour) {
  const float x = ControlX() + Font() * 0.9f;
  ImGui::SetCursorScreenPos(ImVec2(x, ImGui::GetCursorScreenPos().y - Font() * 0.25f));
  ImGui::PushStyleColor(ImGuiCol_Text, colour);
  ImGui::PushTextWrapPos(RightEdge() - ImGui::GetWindowPos().x);
  ImGui::TextUnformatted(text);
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
  return ImGui::IsItemHovered();
}

bool Section(const char* id, const char* title, const char* note, bool* open) {
  Rule();
  ImDrawList* const draw = ImGui::GetWindowDrawList();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float h = RowHeight(), size = Font() * 0.85f;
  if (ImGui::InvisibleButton(id, ImVec2(RightEdge() - at.x, h))) *open = !*open;
  const bool hovered = ImGui::IsItemHovered();
  const float y = at.y + (h - size) * 0.5f;
  float x = at.x + DrawCaps(draw, ImVec2(at.x, y), *open ? "[-]" : "[+]", hovered ? colour::WHITE : colour::DIM, size) + Font() * 0.6f;
  x += DrawCaps(draw, ImVec2(x, y), title, hovered ? colour::WHITE : colour::TEXT, size) + Font() * 0.6f;
  if (note != nullptr && note[0] != '\0') DrawCaps(draw, ImVec2(x, y), note, colour::FAINT, size);
  return *open;
}

}  // namespace hsf::ui
