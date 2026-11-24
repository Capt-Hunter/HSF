// See lzma_dec.h.
#include "lzma_dec.h"

#include <algorithm>
#include <cstring>

namespace hsf::lzma {
namespace {

using Prob = uint16_t;
constexpr Prob kProbInit = 1024;  // half of 1 << 11
constexpr uint32_t kTop = 1u << 24;
constexpr int kStates = 12;
constexpr int kPosBitsMax = 4;
constexpr int kEndPosModel = 14;
constexpr int kFullDistances = 128;
constexpr int kAlignBits = 4;
constexpr uint32_t kMatchMin = 2;

struct Corrupt {};

class RangeDecoder {
 public:
  RangeDecoder(const uint8_t* data, size_t size) : data_(data), size_(size) {
    const uint8_t first = Next();
    for (int i = 0; i < 4; ++i) code_ = (code_ << 8) | Next();
    if (first != 0 || code_ == range_) throw Corrupt{};
  }

  bool FinishedOk() const { return code_ == 0; }

  uint32_t Bit(Prob& prob) {
    const uint32_t bound = (range_ >> 11) * prob;
    uint32_t bit;
    if (code_ < bound) {
      prob = static_cast<Prob>(prob + (((1u << 11) - prob) >> 5));
      range_ = bound;
      bit = 0;
    } else {
      prob = static_cast<Prob>(prob - (prob >> 5));
      code_ -= bound;
      range_ -= bound;
      bit = 1;
    }
    Normalize();
    return bit;
  }

  uint32_t Direct(int count) {
    uint32_t result = 0;
    while (count-- > 0) {
      range_ >>= 1;
      code_ -= range_;
      const uint32_t mask = 0u - (code_ >> 31);  // all ones when the subtraction went below zero
      code_ += range_ & mask;
      if (code_ == range_) throw Corrupt{};
      Normalize();
      result = (result << 1) + (mask + 1);
    }
    return result;
  }

  uint32_t Tree(Prob* probs, int bits) {
    uint32_t m = 1;
    for (int i = 0; i < bits; ++i) m = (m << 1) + Bit(probs[m]);
    return m - (1u << bits);
  }

  uint32_t ReverseTree(Prob* probs, int bits) {
    uint32_t m = 1, symbol = 0;
    for (int i = 0; i < bits; ++i) {
      const uint32_t bit = Bit(probs[m]);
      m = (m << 1) + bit;
      symbol |= bit << i;
    }
    return symbol;
  }

 private:
  uint8_t Next() {
    if (pos_ >= size_) throw Corrupt{};
    return data_[pos_++];
  }
  void Normalize() {
    if (range_ < kTop) {
      range_ <<= 8;
      code_ = (code_ << 8) | Next();
    }
  }

  const uint8_t* data_;
  size_t size_;
  size_t pos_ = 0;
  uint32_t range_ = 0xFFFFFFFFu;
  uint32_t code_ = 0;
};

struct LengthDecoder {
  Prob choice = kProbInit, choice2 = kProbInit;
  Prob low[1 << kPosBitsMax][1 << 3];
  Prob mid[1 << kPosBitsMax][1 << 3];
  Prob high[1 << 8];

  LengthDecoder() {
    std::fill(&low[0][0], &low[0][0] + sizeof(low) / sizeof(Prob), kProbInit);
    std::fill(&mid[0][0], &mid[0][0] + sizeof(mid) / sizeof(Prob), kProbInit);
    std::fill(std::begin(high), std::end(high), kProbInit);
  }

  uint32_t Decode(RangeDecoder& rc, uint32_t pos_state) {
    if (rc.Bit(choice) == 0) return rc.Tree(low[pos_state], 3);
    if (rc.Bit(choice2) == 0) return 8 + rc.Tree(mid[pos_state], 3);
    return 16 + rc.Tree(high, 8);
  }
};

int AfterLiteral(int state) { return state < 4 ? 0 : (state < 10 ? state - 3 : state - 6); }
int AfterMatch(int state) { return state < 7 ? 7 : 10; }
int AfterRep(int state) { return state < 7 ? 8 : 11; }
int AfterShortRep(int state) { return state < 7 ? 9 : 11; }

}  // namespace

bool LooksLikeAlone(const uint8_t* data, size_t size) {
  if (data == nullptr || size < 13 + 5 || data[0] >= 9 * 5 * 5) return false;
  const int lc = data[0] % 9, lp = (data[0] / 9) % 5;
  return lc + lp <= 12;
}

bool DecodeAlone(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t limit, std::string& why) {
  out.clear();
  if (!LooksLikeAlone(data, size)) {
    why = "not an LZMA stream";
    return false;
  }
  const int lc = data[0] % 9, lp = (data[0] / 9) % 5, pb = data[0] / 45;
  uint32_t dictionary;
  uint64_t unpacked;
  memcpy(&dictionary, data + 1, 4);
  memcpy(&unpacked, data + 5, 8);
  dictionary = std::max<uint32_t>(dictionary, 1u << 12);
  const bool sized = unpacked != ~0ull;
  if (sized && unpacked > limit) {
    why = "the unpacked size is too large";
    return false;
  }
  try {
    out.reserve(sized ? static_cast<size_t>(unpacked) : std::min<size_t>(limit, size * 8));
    RangeDecoder rc(data + 13, size - 13);
    std::vector<Prob> literals(size_t{0x300} << (lc + lp), kProbInit);
    Prob is_match[kStates << kPosBitsMax], is_rep[kStates], is_rep_g0[kStates], is_rep_g1[kStates], is_rep_g2[kStates],
        is_rep0_long[kStates << kPosBitsMax];
    Prob pos_slot[4][1 << 6], pos_special[1 + kFullDistances - kEndPosModel], align[1 << kAlignBits];
    for (Prob* group : {is_match, is_rep0_long}) std::fill(group, group + (kStates << kPosBitsMax), kProbInit);
    for (Prob* group : {is_rep, is_rep_g0, is_rep_g1, is_rep_g2}) std::fill(group, group + kStates, kProbInit);
    std::fill(&pos_slot[0][0], &pos_slot[0][0] + 4 * 64, kProbInit);
    std::fill(std::begin(pos_special), std::end(pos_special), kProbInit);
    std::fill(std::begin(align), std::end(align), kProbInit);
    LengthDecoder lengths, rep_lengths;

    const uint32_t pb_mask = (1u << pb) - 1, lp_mask = (1u << lp) - 1;
    uint32_t rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0;  // distances minus one
    int state = 0;
    const auto room = [&](size_t more) {
      if (out.size() + more > limit || (sized && out.size() + more > unpacked)) throw Corrupt{};
    };

    for (;;) {
      if (sized && out.size() == unpacked && rc.FinishedOk()) break;
      const uint32_t pos_state = static_cast<uint32_t>(out.size()) & pb_mask;
      if (rc.Bit(is_match[(state << kPosBitsMax) + pos_state]) == 0) {
        room(1);
        const uint32_t previous = out.empty() ? 0 : out.back();
        Prob* probs = &literals[size_t{0x300} * (((static_cast<uint32_t>(out.size()) & lp_mask) << lc) + (previous >> (8 - lc)))];
        uint32_t symbol = 1;
        if (state >= 7) {
          if (rep0 >= out.size()) throw Corrupt{};
          uint32_t match = out[out.size() - rep0 - 1];
          do {
            const uint32_t match_bit = (match >> 7) & 1;
            match <<= 1;
            const uint32_t bit = rc.Bit(probs[((1 + match_bit) << 8) + symbol]);
            symbol = (symbol << 1) | bit;
            if (match_bit != bit) break;
          } while (symbol < 0x100);
        }
        while (symbol < 0x100) symbol = (symbol << 1) | rc.Bit(probs[symbol]);
        out.push_back(static_cast<uint8_t>(symbol - 0x100));
        state = AfterLiteral(state);
        continue;
      }

      uint32_t length;
      if (rc.Bit(is_rep[state]) != 0) {
        if (out.empty()) throw Corrupt{};
        if (rc.Bit(is_rep_g0[state]) == 0) {
          if (rc.Bit(is_rep0_long[(state << kPosBitsMax) + pos_state]) == 0) {
            room(1);
            if (rep0 >= out.size()) throw Corrupt{};
            const uint8_t repeated = out[out.size() - rep0 - 1];
            out.push_back(repeated);
            state = AfterShortRep(state);
            continue;
          }
        } else {
          uint32_t distance;
          if (rc.Bit(is_rep_g1[state]) == 0) {
            distance = rep1;
          } else {
            if (rc.Bit(is_rep_g2[state]) == 0) {
              distance = rep2;
            } else {
              distance = rep3;
              rep3 = rep2;
            }
            rep2 = rep1;
          }
          rep1 = rep0;
          rep0 = distance;
        }
        length = rep_lengths.Decode(rc, pos_state);
        state = AfterRep(state);
      } else {
        rep3 = rep2;
        rep2 = rep1;
        rep1 = rep0;
        length = lengths.Decode(rc, pos_state);
        state = AfterMatch(state);
        const uint32_t length_state = std::min<uint32_t>(length, 3);
        const uint32_t slot = rc.Tree(pos_slot[length_state], 6);
        if (slot < 4) {
          rep0 = slot;
        } else {
          const int direct = static_cast<int>(slot >> 1) - 1;
          uint32_t distance = (2 | (slot & 1)) << direct;
          if (slot < kEndPosModel) {
            distance += rc.ReverseTree(pos_special + distance - slot, direct);
          } else {
            distance += rc.Direct(direct - kAlignBits) << kAlignBits;
            distance += rc.ReverseTree(align, kAlignBits);
          }
          rep0 = distance;
        }
        if (rep0 == 0xFFFFFFFFu) {  // end marker
          if (!rc.FinishedOk() || (sized && out.size() != unpacked)) throw Corrupt{};
          break;
        }
        if (rep0 >= dictionary || rep0 >= out.size()) throw Corrupt{};
      }
      length += kMatchMin;
      room(length);
      const size_t from = out.size() - rep0 - 1;
      for (uint32_t i = 0; i < length; ++i) {
        const uint8_t copied = out[from + i];  // overlapping copies read bytes just written
        out.push_back(copied);
      }
    }
  } catch (const Corrupt&) {
    why = out.size() >= limit ? "the unpacked data is too large" : "the LZMA stream is corrupt";
    out.clear();
    return false;
  } catch (const std::bad_alloc&) {
    why = "out of memory";
    out.clear();
    return false;
  }
  return true;
}

}  // namespace hsf::lzma
