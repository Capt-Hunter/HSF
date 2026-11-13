// HSF's menu look: the same design language as a typical RTSS overlay and the game HUDs it follows. A near-black panel with a thin light border and
// an orange corner bracket, small spaced capitals for headers, one colour per row label, big condensed numbers, thin bars with a white tick, and
// outlined boxes for choices (the chosen one bright, the rest dim). Drawn with ReShade's Dear ImGui: the big numbers are segment shapes, so they stay
// sharp at any size (ReShade owns the font, a small bitmap that blurs when scaled up).
#pragma once

#include <imgui.h>

#include <cstddef>
#include <cstdint>

namespace hsf::ui {

namespace colour {
// Dark green-grey, as a typical RTSS overlay.
constexpr ImU32 PANEL = IM_COL32(20, 28, 23, 244);
constexpr ImU32 BORDER = IM_COL32(74, 90, 79, 220);
constexpr ImU32 LINE = IM_COL32(50, 64, 55, 220);
constexpr ImU32 BRACKET = IM_COL32(255, 110, 40, 255);
constexpr ImU32 TEXT = IM_COL32(232, 236, 230, 255);
constexpr ImU32 DIM = IM_COL32(136, 150, 141, 255);
constexpr ImU32 FAINT = IM_COL32(72, 86, 77, 255);
constexpr ImU32 GREEN = IM_COL32(120, 205, 60, 255);
constexpr ImU32 RED = IM_COL32(240, 82, 70, 255);
constexpr ImU32 BLUE = IM_COL32(110, 160, 240, 255);
constexpr ImU32 PURPLE = IM_COL32(178, 142, 240, 255);
constexpr ImU32 AMBER = IM_COL32(255, 180, 40, 255);
constexpr ImU32 WHITE = IM_COL32(255, 255, 255, 255);
}  // namespace colour

// The whole menu sits in one panel. Always call EndPanel after BeginPanel.
void BeginPanel(const char* id);
void EndPanel();

// "HSF // FRAME GEN" on the left, `right` (card, API) on the right, a thin line under both.
void Header(const char* left, const char* right);
// A thin line across the panel.
void Rule();

// Text in spaced capitals, `scale` times ReShade's font size, at the cursor. Returns its width.
float Caps(const char* text, ImU32 colour, float scale = 0.85f);
// Small text on one line, at the cursor, then a new line.
void Small(const char* text, ImU32 colour = colour::DIM);
// The same, wrapped to the panel's width.
void SmallWrapped(const char* text, ImU32 colour = colour::DIM);

// Digits, '.', 'x' and spaces as segment shapes, `h` high, at `at`; returns their width. With `draw` null it only measures.
float SegmentText(ImDrawList* draw, ImVec2 at, float h, const char* text, ImU32 colour);
// A big number with two small lines beside it ("FPS OUT", "72 BASE // 13.9 MS") and, at the right, an outlined tag ("GENERATING").
void BigReadout(const char* number, ImU32 number_colour, const char* line1, const char* line2, const char* tag, ImU32 tag_colour);

// A row's coloured label on the left; what follows on the same line starts at the panel's control column.
void RowLabel(const char* text, ImU32 colour);
// The control column's x, for rows that place things themselves.
float ControlX();

// Outlined boxes, one per name; true when the choice changed. Bit i of `disabled` greys out and blocks box i;
// `tips`, if given, holds each box's tooltip for when the mouse is over it.
bool Choice(const char* id, const char* const* names, int count, int* value, uint32_t disabled = 0, const char* const* tips = nullptr);
// ON / OFF as two boxes.
bool Toggle(const char* id, bool* value);
// An outlined button; true when clicked. `accent` colours its border and text.
bool Button(const char* id, const char* text, ImU32 accent = colour::TEXT);
// One line of small capitals in `colour`, cut at the panel's edge.
void Note(const char* text, ImU32 colour);
// An outlined button that stays lit while `lit` (OFF on the FPS cap while there is no cap).
bool LitButton(const char* id, const char* text, bool lit);
// `colour` with its alpha pulsing about once a second, never below a third, so pulsing text stays readable. For warnings the player must notice.
ImU32 Pulsing(ImU32 colour);
// A filled warning triangle with a "!" cut out of it, `size` high, centred on `centre`.
void WarningIcon(ImVec2 centre, float size, ImU32 colour);
// A small outlined key name ("F7"), at the right edge of the panel on the current line.
void KeyChip(const char* text);
// Dim text at the right edge of the panel on the current line.
void RightNote(const char* text, ImU32 colour = colour::DIM);

// A thin bar with a white tick at `value`, `width` wide (0 = to the control column's end); dragging sets it. `released`: let go this frame.
bool Bar(const char* id, int* value, int min, int max, float width, ImU32 fill, bool* released);

// An outlined box across the panel: `text` on the left, `verdict` on the right in `verdict_colour`.
void StatusBox(const char* text, const char* verdict, ImU32 verdict_colour);
// A red box across the panel that pulses, `title` big and `line` under it, both centred. For a warning the player must not miss.
// `button` is drawn big inside the box, under the text; true the frame it is clicked. nullptr: no button. `tint`: red for must-do, orange for advice.
bool AlertBox(const char* id, const char* title, const char* line, const char* button, ImU32 tint = IM_COL32(255, 60, 60, 255));
// Frame times as a thin line across the panel, `height` high.
void Sparkline(const float* values, size_t count, float height);

// "[-] ADVANCED" / "[+] ADVANCED" with `note` dim after it; clicking flips `open`. Returns `open`.
bool Section(const char* id, const char* title, const char* note, bool* open);

// A row for Advanced and Debug: a dim label, the control at the control column.
void SettingLabel(const char* text);

}  // namespace hsf::ui
