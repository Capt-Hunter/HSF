// A small LZMA decoder for the ".lzma" (LZMA-alone) format: a 13-byte header (properties byte, dictionary size, unpacked size, which may be
// "unknown" with an end marker) followed by one LZMA stream. HSF's own code, written from the format as published with the public-domain
// LZMA SDK (Igor Pavlov); no SDK code is copied. Used to read the shader programs dashdogy's MFG Unlocker packs in its resources
// (xmx_model.cpp). Pure memory work: no allocation beyond the output vector, no I/O.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hsf::lzma {

// Decodes one LZMA-alone stream. Stops with false (and a reason) on any corrupt input or once the output would pass `limit` bytes.
bool DecodeAlone(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t limit, std::string& why);

// True when the bytes start like an LZMA-alone header HSF can decode (lc+lp <= 12 as the SDK allows, pb <= 4).
bool LooksLikeAlone(const uint8_t* data, size_t size);

}  // namespace hsf::lzma
