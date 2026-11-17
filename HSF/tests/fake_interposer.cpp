// A stand-in sl.interposer.dll for sl_copies_test: the exports HSF hooks, with no Streamline behind them. sl_copies_test copies it into two folders
// as sl.interposer.dll and loads both, as Gothic Remake did with RenoDX DLSS's stray copy and the game's own. A copy in a folder whose name starts with
// "stray" acts as one the game never started (slIsFeatureLoaded answers eErrorNotInitialized); any other copy acts as started.
#include <windows.h>

#include <cstdint>
#include <cwchar>

#include <sl.h>

namespace {

struct Token final : sl::FrameToken {
  uint32_t index = 0;
  operator uint32_t() const override { return index; }
};
Token g_token;
volatile uint32_t g_calls = 0;
bool g_stray = false;

}  // namespace

// Built without optimisation, so each export opens with an ordinary prologue long enough for HSF's hook (a one-line body compiles to two bytes).
#pragma optimize("", off)

sl::Result slSetTag(const sl::ViewportHandle& viewport, const sl::ResourceTag* tags, uint32_t count, sl::CommandBuffer* list) {
  g_calls = g_calls + 1 + (&viewport != nullptr) + (tags != nullptr) + count + (list != nullptr);
  return sl::Result::eOk;
}
sl::Result slSetTagForFrame(const sl::FrameToken& frame, const sl::ViewportHandle& viewport, const sl::ResourceTag* tags, uint32_t count, sl::CommandBuffer* list) {
  g_calls = g_calls + 1 + (&frame != nullptr) + (&viewport != nullptr) + (tags != nullptr) + count + (list != nullptr);
  return sl::Result::eOk;
}
sl::Result slSetConstants(const sl::Constants& values, const sl::FrameToken& frame, const sl::ViewportHandle& viewport) {
  g_calls = g_calls + 1 + (&values != nullptr) + (&frame != nullptr) + (&viewport != nullptr);
  return sl::Result::eOk;
}
sl::Result slEvaluateFeature(sl::Feature feature, const sl::FrameToken& frame, const sl::BaseStructure** inputs, uint32_t count, sl::CommandBuffer* list) {
  g_calls = g_calls + 1 + feature + (&frame != nullptr) + (inputs != nullptr) + count + (list != nullptr);
  return sl::Result::eOk;
}
sl::Result slGetNewFrameToken(sl::FrameToken*& token, const uint32_t* index) {
  g_calls = g_calls + 1;
  g_token.index = index != nullptr ? *index : g_token.index + 1;
  token = &g_token;
  return sl::Result::eOk;
}
sl::Result slGetFeatureFunction(sl::Feature feature, const char* name, void*& function) {
  g_calls = g_calls + 1 + feature + (name != nullptr);
  function = nullptr;
  return sl::Result::eErrorFeatureMissing;
}
sl::Result slSetFeatureLoaded(sl::Feature feature, bool loaded) {
  g_calls = g_calls + 1 + feature + (loaded ? 1 : 0);
  return sl::Result::eOk;
}
sl::Result slIsFeatureLoaded(sl::Feature feature, bool& loaded) {
  g_calls = g_calls + 1 + feature;
  loaded = false;
  return g_stray ? sl::Result::eErrorNotInitialized : sl::Result::eOk;
}

#pragma optimize("", on)

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    wchar_t path[MAX_PATH] = L"";
    GetModuleFileNameW(module, path, MAX_PATH);
    for (wchar_t* c = path; *c != 0; ++c) *c = static_cast<wchar_t>(towlower(*c));
    g_stray = std::wcsstr(path, L"\\stray") != nullptr;
  }
  return TRUE;
}
