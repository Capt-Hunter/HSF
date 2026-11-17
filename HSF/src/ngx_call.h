// One reading of a DLSS call for all three NGX hooks (Direct3D 12 and 11 in ngx_input.cpp, Vulkan in ngx_vk.cpp). What a call says about its
// vectors (the part in use, their scale, jitter, flags and reset) is read here by one rule; each hook keeps only its own copying.
#pragma once

#include <cstdint>

#include "ngx_input.h"

namespace hsf::ngx::call {

constexpr int NGX_SUCCESS = 1;
constexpr int FLAG_MV_LOW_RES = 1 << 1, FLAG_MV_JITTERED = 1 << 2, FLAG_DEPTH_INVERTED = 1 << 3;
constexpr int FEATURE_SUPER_SAMPLING = 1, FEATURE_RAY_RECONSTRUCTION = 13;

inline float ReadFloat(const Parameter* parameters, const char* name, float otherwise) {
  float value = 0.f;
  return parameters->Get(name, &value) == NGX_SUCCESS ? value : otherwise;
}
inline uint32_t ReadUint(const Parameter* parameters, const char* name) {
  unsigned int value = 0;
  return parameters->Get(name, &value) == NGX_SUCCESS ? value : 0;
}

// What the last CreateFeature for DLSS said, the only sure place for the flags and sizes. flags -1: not known.
struct Created {
  int flags = -1;
  uint32_t width = 0, height = 0, out_width = 0, out_height = 0;
};
// After a CreateFeature that worked: true for DLSS's upscalers (super sampling, ray reconstruction), with what it said in `into`. Flags already known
// are kept when this call has none.
inline bool ReadCreate(const Parameter* parameters, int feature, Created& into) {
  if (parameters == nullptr || (feature != FEATURE_SUPER_SAMPLING && feature != FEATURE_RAY_RECONSTRUCTION)) return false;
  int flags = 0;
  if (parameters->Get("DLSS.Feature.Create.Flags", &flags) == NGX_SUCCESS) into.flags = flags;
  into.width = ReadUint(parameters, "Width");
  into.height = ReadUint(parameters, "Height");
  into.out_width = ReadUint(parameters, "OutWidth");
  into.out_height = ReadUint(parameters, "OutHeight");
  return true;
}

struct DlssCall {
  uint32_t x = 0, y = 0, width = 0, height = 0;  // the part of the vectors in use, which can change every frame with dynamic resolution
  uint32_t render_width = 0, render_height = 0;  // the render size: the part of the depth in use
  float to_pixels[2] = {1.f, 1.f};               // NGX's MV.Scale: a vector times this is in pixels of the part in use
  float jitter[2] = {};                          // in pixels of the render size
  int flags = -1;                                // DLSS's create flags, -1 not known
  bool low_res = true;                           // the vectors are at the render size; taken when the flags are not known
  bool jittered = false;                         // the vectors include the jitter
  int depth_inverted = -1;                       // 1 reversed, 0 normal, -1 not known
  bool reset = false;
  uint64_t Area() const { return static_cast<uint64_t>(width) * height; }
};

// One call's parameters, with what CreateFeature said for what the call leaves out, and the size of the vectors' texture to keep the part in use
// inside it.
inline DlssCall Read(const Parameter* parameters, const Created& created, uint32_t motion_width, uint32_t motion_height) {
  DlssCall c;
  c.flags = created.flags;
  if (c.flags < 0 && parameters->Get("DLSS.Feature.Create.Flags", &c.flags) != NGX_SUCCESS) c.flags = -1;
  const bool flags_known = c.flags >= 0;
  c.low_res = !flags_known || (c.flags & FLAG_MV_LOW_RES) != 0;
  c.jittered = flags_known && (c.flags & FLAG_MV_JITTERED) != 0;
  c.depth_inverted = flags_known ? ((c.flags & FLAG_DEPTH_INVERTED) != 0 ? 1 : 0) : -1;

  // The render size: this call's subrect, else the size DLSS was made for, else the whole texture.
  c.render_width = ReadUint(parameters, "DLSS.Render.Subrect.Dimensions.Width");
  c.render_height = ReadUint(parameters, "DLSS.Render.Subrect.Dimensions.Height");
  if (c.render_width == 0 || c.render_height == 0) {
    c.render_width = created.width;
    c.render_height = created.height;
  }
  if (c.render_width == 0 || c.render_height == 0) {
    c.render_width = motion_width;
    c.render_height = motion_height;
  }
  // The vectors' part in use: the render size for low-resolution vectors, otherwise the output.
  c.width = c.render_width;
  c.height = c.render_height;
  if (!c.low_res && created.out_width != 0 && created.out_height != 0) {
    c.width = created.out_width;
    c.height = created.out_height;
  }
  if (c.width == 0 || c.height == 0 || c.width > motion_width || c.height > motion_height) {
    c.width = motion_width;
    c.height = motion_height;
  }
  c.x = ReadUint(parameters, "DLSS.Input.MV.Subrect.Base.X");
  c.y = ReadUint(parameters, "DLSS.Input.MV.Subrect.Base.Y");
  if (c.x + c.width > motion_width || c.y + c.height > motion_height) c.x = c.y = 0;

  c.to_pixels[0] = ReadFloat(parameters, "MV.Scale.X", 1.f);
  c.to_pixels[1] = ReadFloat(parameters, "MV.Scale.Y", 1.f);
  c.jitter[0] = ReadFloat(parameters, "Jitter.Offset.X", 0.f);
  c.jitter[1] = ReadFloat(parameters, "Jitter.Offset.Y", 0.f);
  int reset = 0;
  c.reset = parameters->Get("Reset", &reset) == NGX_SUCCESS && reset != 0;
  return c;
}

// The part of the depth in use: the render size, inside the depth image, at the call's depth subrect.
struct DepthPart {
  uint32_t x = 0, y = 0, width = 0, height = 0;
};
inline DepthPart ReadDepthPart(const Parameter* parameters, const DlssCall& c, uint32_t depth_width, uint32_t depth_height) {
  DepthPart d;
  d.width = c.render_width <= depth_width ? c.render_width : depth_width;
  d.height = c.render_height <= depth_height ? c.render_height : depth_height;
  d.x = ReadUint(parameters, "DLSS.Input.Depth.Subrect.Base.X");
  d.y = ReadUint(parameters, "DLSS.Input.Depth.Subrect.Base.Y");
  if (d.x + d.width > depth_width || d.y + d.height > depth_height) d.x = d.y = 0;
  return d;
}

// A game can call DLSS for something small too, such as a mirror: a call is the main one when it is not less than half the largest of the last
// 240 calls. `now` counts the calls (each hook its own).
struct MainCall {
  uint64_t largest_area = 0, largest_at = 0;
  bool Take(uint64_t area, uint64_t now) {
    if (now - largest_at > 240) largest_area = 0;
    if (area * 2 < largest_area) return false;
    if (area >= largest_area) {
      largest_area = area;
      largest_at = now;
    }
    return true;
  }
};

}  // namespace hsf::ngx::call
