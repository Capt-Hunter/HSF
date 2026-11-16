// The motion score's thresholds, shared by score.cpp (D3D11, in the add-on) and frame_checks.cpp (D3D12 and the helper), so both rate alike.
#pragma once

#include <cstdint>

namespace hsf::scorelimits {

constexpr uint32_t GRID_WIDTH = 240;
constexpr float MOVING_ERROR = 2.f / 255.f;
constexpr float CLEARLY = 0.1f;
constexpr float STILL_SHARE = 0.01f;
constexpr int HISTOGRAM_BINS = 32;
constexpr float CUT_HISTOGRAM = 0.35f;
constexpr float CUT_GAIN = 0.2f;

}  // namespace hsf::scorelimits
