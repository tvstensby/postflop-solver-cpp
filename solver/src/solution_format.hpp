// Constants and byte-level helpers of the solution file format ("postflop-solver/solution"),
// shared by the writer (solution_file.hpp) and the reader (solution_reader.hpp). The format is
// specified in documentation/SolverFileFormat.md; the section numbers below refer to it.
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace pfs_solver::solution {

// §3: header.
inline constexpr std::array<uint8_t, 8> kMagic{0x89, 'P', 'F', 'S', 'O', 'L', '\r', '\n'};
inline constexpr uint16_t kVersionMajor = 1;
// 1.1 added riskPremiumPercent and bubbleFactor to the metadata, which older readers ignore.
inline constexpr uint16_t kVersionMinor = 1;
inline constexpr uint32_t kFlagCrc = 1;
inline constexpr size_t kHeaderSize = 72;

// §2: common structures.
inline constexpr size_t kBlobRefSize = 16;
inline constexpr size_t kSegmentRefSize = 24;
inline constexpr size_t kChanceTableSize = 52 * kSegmentRefSize;
inline constexpr uint8_t kIdentityPerm = 0xE4;

// §4: blob envelope.
inline constexpr size_t kBlobEnvelopeSize = 8;

// §8: segment table.
inline constexpr uint32_t kSegmentMagic = 0x54474553;  // "SEGT"
inline constexpr size_t kSegmentHeaderSize = 24;

// §9: node data.
inline constexpr uint32_t kNodeMagic = 0x45444F4E;  // "NODE"
inline constexpr size_t kNodeHeaderSize = 16;
inline constexpr size_t kFieldDescSize = 16;
inline constexpr uint8_t kNoPlayer = 0xFF;
inline constexpr size_t kSummaryFixed = 6;  // ev, equity and combos of both players

enum class BlobType : uint8_t { Metadata = 1, Node = 2 };
enum class Codec : uint8_t { None = 0, Zlib = 1, Zstd = 2 };
enum class EntryKind : uint8_t { Impossible = 0, Segment = 1, Empty = 2, NotStored = 3, Pruned = 4 };
enum class SlotState : uint8_t { Stored = 0, NotStored = 1, Pruned = 2 };
enum class FieldId : uint8_t {
    Strategy = 1,
    ActionEv = 2,
    Weight = 3,
    NormalizedWeight = 4,
    Ev = 5,
    Equity = 6,
    Summary = 7,
};
enum class Encoding : uint8_t { F32 = 0, U16Unit = 1, I16Scaled = 2, U16Scaled = 3 };
enum class Filter : uint8_t { None = 0, ByteShuffle = 1 };

struct BlobRef {
    uint64_t offset = 0;
    uint32_t length = 0;  // envelope included, 0 = no data
    uint32_t crc32 = 0;
};

struct SegmentRef {
    uint64_t offset = 0;
    uint32_t table_length = 0;
    uint32_t span_length = 0;
    uint32_t crc32 = 0;
    EntryKind kind = EntryKind::Impossible;
    uint8_t suit_perm = 0;
};

inline constexpr size_t pad8(size_t n) noexcept { return (n + 7) & ~size_t{7}; }

inline constexpr size_t segment_table_size(size_t num_slots, size_t num_exits) noexcept {
    return kSegmentHeaderSize + kBlobRefSize * num_slots + pad8(num_slots) +
           kChanceTableSize * num_exits;
}

inline constexpr size_t encoding_size(Encoding encoding) noexcept {
    return encoding == Encoding::F32 ? 4 : 2;
}

// ---- little-endian access ----

inline void put_u8(uint8_t* p, uint8_t v) noexcept { p[0] = v; }
inline void put_u16(uint8_t* p, uint16_t v) noexcept {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
inline void put_u32(uint8_t* p, uint32_t v) noexcept {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
inline void put_u64(uint8_t* p, uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
inline void put_f32(uint8_t* p, float v) noexcept { put_u32(p, std::bit_cast<uint32_t>(v)); }

inline uint16_t get_u16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
inline uint32_t get_u32(const uint8_t* p) noexcept {
    uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
inline uint64_t get_u64(const uint8_t* p) noexcept {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
inline float get_f32(const uint8_t* p) noexcept { return std::bit_cast<float>(get_u32(p)); }

inline void put_blob_ref(uint8_t* p, const BlobRef& r) noexcept {
    put_u64(p, r.offset);
    put_u32(p + 8, r.length);
    put_u32(p + 12, r.crc32);
}
inline BlobRef get_blob_ref(const uint8_t* p) noexcept {
    return {get_u64(p), get_u32(p + 8), get_u32(p + 12)};
}

inline void put_segment_ref(uint8_t* p, const SegmentRef& r) noexcept {
    put_u64(p, r.offset);
    put_u32(p + 8, r.table_length);
    put_u32(p + 12, r.span_length);
    put_u32(p + 16, r.crc32);
    put_u8(p + 20, static_cast<uint8_t>(r.kind));
    put_u8(p + 21, r.suit_perm);
    put_u16(p + 22, 0);
}
inline SegmentRef get_segment_ref(const uint8_t* p) noexcept {
    return {get_u64(p),   get_u32(p + 8), get_u32(p + 12), get_u32(p + 16),
            static_cast<EntryKind>(p[20]), p[21]};
}

// ---- suit permutations (§6) ----

inline uint8_t perm_suit(uint8_t perm, uint8_t suit) noexcept {
    return static_cast<uint8_t>((perm >> (2 * suit)) & 3);
}
inline uint8_t perm_card(uint8_t perm, uint8_t card) noexcept {
    return static_cast<uint8_t>((card & ~3) | perm_suit(perm, card & 3));
}
// sigma ∘ pi: apply pi first, then sigma.
inline uint8_t perm_compose(uint8_t sigma, uint8_t pi) noexcept {
    uint8_t out = 0;
    for (uint8_t s = 0; s < 4; ++s)
        out = static_cast<uint8_t>(out | (perm_suit(sigma, perm_suit(pi, s)) << (2 * s)));
    return out;
}
inline uint8_t perm_transposition(uint8_t suit1, uint8_t suit2) noexcept {
    uint8_t out = 0;
    for (uint8_t s = 0; s < 4; ++s) {
        const uint8_t t = s == suit1 ? suit2 : s == suit2 ? suit1 : s;
        out = static_cast<uint8_t>(out | (t << (2 * s)));
    }
    return out;
}

// ---- byte shuffle (§9), which the writer does not use ----

inline void byte_unshuffle(uint8_t* data, size_t count, size_t width) {
    std::vector<uint8_t> tmp(data, data + count * width);
    for (size_t i = 0; i < count; ++i)
        for (size_t b = 0; b < width; ++b) data[i * width + b] = tmp[b * count + i];
}

}  // namespace pfs_solver::solution
