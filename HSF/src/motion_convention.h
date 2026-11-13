// What a motion source's texel values mean, and how to turn them into frame pixels. No graphics API in here, so the tests can use it.
#pragma once

namespace hsf::motion {

// Convention c (1-6) turns a texel value v into the offset, in frame pixels, from a pixel to where the same point was one frame earlier:
// v * sign * unit, sign + for odd c, unit = frame size (UV, c 1-2), frame size / motion texture size (pixels of the motion texture, c 3-4) or
// 1 (frame pixels, c 5-6). 0 = no motion.
constexpr int CONVENTIONS = 6;
constexpr const char* CONVENTION_NAMES[CONVENTIONS + 1] = {"none", "uv", "-uv", "mv pixels", "-mv pixels", "pixels", "-pixels"};
inline void ConventionScale(int convention, float width, float height, float motion_width, float motion_height, float out[2]) {
  const float sign = convention % 2 == 1 ? 1.f : -1.f;
  const int unit = (convention - 1) / 2;
  out[0] = convention == 0 ? 0.f : sign * (unit == 0 ? width : unit == 1 ? width / motion_width : 1.f);
  out[1] = convention == 0 ? 0.f : sign * (unit == 0 ? height : unit == 1 ? height / motion_height : 1.f);
}

}  // namespace hsf::motion
