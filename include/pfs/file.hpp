// Port of src/file.rs plus the storage-mode half of src/game/serialization.rs.
//
// File layout (all integers fixed-width little-endian, unlike the Rust which uses
// bincode varints):
//
//   magic              u32   0x43534650 ("PFSC")
//   format version     u16   1
//   compression type   u8    0 = none, 1 = zstd
//   data type          u8    0 = game
//   estimated memory   u64   checked against max_memory_usage BEFORE the payload
//   memo               u64 length + bytes
//   payload                  (optionally zstd-compressed)
//
// The magic differs from the Rust's 0x09f15790 on purpose: a file written by the
// Rust solver must be rejected cleanly, not mis-parsed.
#pragma once

#include <pfs/common.hpp>
#include <pfs/game.hpp>
#include <pfs/result.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pfs {

inline constexpr uint32_t kFileMagic = 0x43534650;  // "PFSC"
inline constexpr uint16_t kFileFormatVersion = 1;

enum class DataType : uint8_t { Game = 0, Bunching = 1 };

struct LoadedGame {
    std::unique_ptr<PostFlopGame> game;
    std::string memo;
};

// `compression_level` requires PFS_ENABLE_ZSTD; without it, passing a level is an
// error, mirroring the Rust behaviour when the zstd feature is off.
Status save_game_to_file(const PostFlopGame& game, const std::string& memo,
                         const std::string& path,
                         std::optional<int> compression_level = std::nullopt);

Result<LoadedGame> load_game_from_file(const std::string& path,
                                      std::optional<uint64_t> max_memory_usage = std::nullopt);

// In-memory variants, useful for tests and for callers with their own I/O.
Result<std::vector<std::byte>> save_game_to_bytes(
    const PostFlopGame& game, const std::string& memo,
    std::optional<int> compression_level = std::nullopt);

Result<LoadedGame> load_game_from_bytes(const std::byte* data, size_t size,
                                       std::optional<uint64_t> max_memory_usage = std::nullopt);

}  // namespace pfs
