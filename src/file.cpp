#include <pfs/file.hpp>

#include "io/binary_archive.hpp"

#include <cstdio>
#include <fstream>

#if PFS_ENABLE_ZSTD
#include <zstd.h>
#endif

namespace pfs {

namespace {

Status write_all(const std::string& path, const std::vector<std::byte>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return Status::err("Failed to open file for writing: " + path);
    if (!bytes.empty())
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    if (!out) return Status::err("Failed to write data: " + path);
    return Status::ok();
}

Result<std::vector<std::byte>> read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return Result<std::vector<std::byte>>::err("Failed to open file: " + path);
    const std::streamoff size = in.tellg();
    if (size < 0) return Result<std::vector<std::byte>>::err("Failed to size file: " + path);
    in.seekg(0);
    std::vector<std::byte> bytes(static_cast<size_t>(size));
    if (size && !in.read(reinterpret_cast<char*>(bytes.data()), size))
        return Result<std::vector<std::byte>>::err("Failed to read data: " + path);
    return bytes;
}

}  // namespace

Result<std::vector<std::byte>> save_game_to_bytes(const PostFlopGame& game,
                                                const std::string& memo,
                                                std::optional<int> compression_level) {
    if (!game.is_solved())
        return Result<std::vector<std::byte>>::err("Data is not ready to save");

#if !PFS_ENABLE_ZSTD
    if (compression_level.has_value())
        return Result<std::vector<std::byte>>::err("Compression is not supported");
#endif

    BinWriter payload;
    game.serialize(payload);
    const std::vector<std::byte> raw = payload.take();

    BinWriter header;
    header.u32(kFileMagic);
    header.u16(kFileFormatVersion);
    header.u8(compression_level.has_value() ? 1 : 0);
    header.u8(static_cast<uint8_t>(DataType::Game));
    // Ahead of the payload so a loader can reject an oversized file before reading it.
    header.u64(game.target_memory_usage());
    header.str(memo);

    std::vector<std::byte> out = header.take();

#if PFS_ENABLE_ZSTD
    if (compression_level.has_value()) {
        const size_t bound = ZSTD_compressBound(raw.size());
        std::vector<std::byte> compressed(bound);
        const size_t written = ZSTD_compress(compressed.data(), bound, raw.data(), raw.size(),
                                             *compression_level);
        if (ZSTD_isError(written))
            return Result<std::vector<std::byte>>::err(std::string("zstd compression failed: ") +
                                                      ZSTD_getErrorName(written));
        BinWriter tail;
        tail.u64(raw.size());  // uncompressed size, for the decoder
        tail.blob(compressed.data(), written);
        const std::vector<std::byte> t = tail.take();
        out.insert(out.end(), t.begin(), t.end());
        return out;
    }
#endif

    BinWriter tail;
    tail.blob(raw.data(), raw.size());
    const std::vector<std::byte> t = tail.take();
    out.insert(out.end(), t.begin(), t.end());
    return out;
}

Result<LoadedGame> load_game_from_bytes(const std::byte* data, size_t size,
                                       std::optional<uint64_t> max_memory_usage) {
    BinReader r(data, size);

    if (r.u32() != kFileMagic) return Result<LoadedGame>::err("Magic number is invalid");
    if (r.u16() != kFileFormatVersion) return Result<LoadedGame>::err("Version number is invalid");

    const uint8_t compression_type = r.u8();
    if (compression_type > 1) return Result<LoadedGame>::err("Compression type is invalid");
#if !PFS_ENABLE_ZSTD
    if (compression_type == 1) return Result<LoadedGame>::err("Compression is not supported");
#endif

    if (r.u8() != static_cast<uint8_t>(DataType::Game))
        return Result<LoadedGame>::err("Data type is invalid");

    const uint64_t estimated = r.u64();
    if (max_memory_usage && estimated > *max_memory_usage)
        return Result<LoadedGame>::err("Estimated memory usage is too large");

    const std::string memo = r.str();
    if (!r.ok()) return Result<LoadedGame>::err("Corrupt data: header");

    std::vector<std::byte> payload;
    if (compression_type == 0) {
        payload = r.vec<std::byte>();
        if (!r.ok()) return Result<LoadedGame>::err("Corrupt data: payload");
    } else {
#if PFS_ENABLE_ZSTD
        const uint64_t uncompressed_size = r.u64();
        const std::vector<std::byte> compressed = r.vec<std::byte>();
        if (!r.ok()) return Result<LoadedGame>::err("Corrupt data: payload");
        payload.resize(static_cast<size_t>(uncompressed_size));
        const size_t written = ZSTD_decompress(payload.data(), payload.size(), compressed.data(),
                                              compressed.size());
        if (ZSTD_isError(written) || written != payload.size())
            return Result<LoadedGame>::err("zstd decompression failed");
#else
        return Result<LoadedGame>::err("Compression is not supported");
#endif
    }

    BinReader pr(payload.data(), payload.size());
    Result<std::unique_ptr<PostFlopGame>> game = PostFlopGame::deserialize(pr);
    if (!game) return Result<LoadedGame>::err(game.error());

    LoadedGame out;
    out.game = std::move(game.value());
    out.memo = memo;
    return out;
}

Status save_game_to_file(const PostFlopGame& game, const std::string& memo,
                        const std::string& path, std::optional<int> compression_level) {
    Result<std::vector<std::byte>> bytes = save_game_to_bytes(game, memo, compression_level);
    if (!bytes) return Status::err(bytes.error());
    return write_all(path, bytes.value());
}

Result<LoadedGame> load_game_from_file(const std::string& path,
                                     std::optional<uint64_t> max_memory_usage) {
    Result<std::vector<std::byte>> bytes = read_all(path);
    if (!bytes) return Result<LoadedGame>::err(bytes.error());
    return load_game_from_bytes(bytes.value().data(), bytes.value().size(), max_memory_usage);
}

}  // namespace pfs
