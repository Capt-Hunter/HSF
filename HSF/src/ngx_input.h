// The game's own DLSS data, for frame generation. A game that calls DLSS through NGX, with no Streamline, hands the driver's _nvngx.dll its motion vectors, depth, jitter and
// reset every frame. HSF hooks that call, always calls through so the game's real DLSS still runs, and copies the vectors and depth on the game's command list just after it. Direct3D 12 only.
#pragma once

#include <d3d12.h>

#include <cstdint>

struct ID3D11Resource;
struct ID3D11ShaderResourceView;

namespace hsf::ngx {

using LogFunction = void (*)(const char* text);

void Start(LogFunction log);
// Often, from the thread that presents: hooks _nvngx.dll once the game has loaded it.
void Poll();
void Stop(bool process_exiting);

// What the last DLSS call gave. The textures are HSF's copies, readable by any shader, and hold the data of the last call that has run on the GPU, as HSF's list runs after the game's on the same queue.
struct Frame {
  ID3D12Resource* motion = nullptr;
  ID3D12Resource* depth = nullptr;  // R32_FLOAT, the same size as motion when usable
  uint32_t x = 0, y = 0, width = 0, height = 0;  // the part of the textures in use, which can change every frame with dynamic resolution
  float scale[2] = {};    // DLSS-G's mvecScale
  float jitter[2] = {};   // in pixels of the render size
  bool jittered = false;  // the vectors include the jitter
  bool full_res = false;  // the vectors are at the size of the output, already dilated
  bool depth_inverted = false;
  bool reset = false;
  uint64_t serial = 0;    // goes up with each call taken
};
// False until a call has been taken. The textures stay alive for at least 16 more calls.
bool Latest(Frame* out);

// The parameter block NGX passes, the same declarations in the same order as the SDK's, so the compiler lays out the same method table.
struct Parameter {
  virtual void Set(const char* name, unsigned long long value) = 0;
  virtual void Set(const char* name, float value) = 0;
  virtual void Set(const char* name, double value) = 0;
  virtual void Set(const char* name, unsigned int value) = 0;
  virtual void Set(const char* name, int value) = 0;
  virtual void Set(const char* name, ID3D11Resource* value) = 0;
  virtual void Set(const char* name, ID3D12Resource* value) = 0;
  virtual void Set(const char* name, void* value) = 0;
  virtual int Get(const char* name, unsigned long long* value) const = 0;
  virtual int Get(const char* name, float* value) const = 0;
  virtual int Get(const char* name, double* value) const = 0;
  virtual int Get(const char* name, unsigned int* value) const = 0;
  virtual int Get(const char* name, int* value) const = 0;
  virtual int Get(const char* name, ID3D11Resource** value) const = 0;
  virtual int Get(const char* name, ID3D12Resource** value) const = 0;
  virtual int Get(const char* name, void** value) const = 0;
  virtual void Reset() = 0;
};
// What the hook does after a DLSS call that worked. Public for the bench test.
void Capture(ID3D12GraphicsCommandList* list, const Parameter* parameters);
// How many of the D3D12 calls got hooked; what the bench test checks against the real DLL.
int HookedCalls();

// Direct3D 11: a mod that adds DLSS to a D3D11 game (Luma) calls NGX's D3D11 functions. HSF hooks them the same way, always calls through, and
// copies the vectors and depth on the same context just after each call.
void Start11(LogFunction log);
void Poll11();  // often, from the thread that runs the effects
void Stop11(bool process_exiting);
// What the last D3D11 DLSS call gave: views of HSF's copies, alive until the next call of another size or format.
struct Frame11 {
  ID3D11ShaderResourceView* motion = nullptr;
  ID3D11ShaderResourceView* depth = nullptr;
  uint32_t x = 0, y = 0, width = 0, height = 0;  // the part of the vectors in use
  float scale[2] = {};  // NGX's MV.Scale: a vector times this is in pixels of the part in use
  bool reset = false;
  uint64_t serial = 0;  // goes up with each call taken
};
bool Latest11(Frame11* out);

}  // namespace hsf::ngx
