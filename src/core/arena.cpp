#include "arena.hpp"

#include <algorithm>

namespace pfs {

void Arena::release() {
    for (Chunk& c : chunks_) ::operator delete(c.base, std::align_val_t{kAlignment});
    chunks_.clear();
    chunks_.shrink_to_fit();
    chunk_index_ = kNoChunk;
    cursor_ = 0;
}

void Arena::grow(size_t min_bytes) {
    // Advance to the next existing chunk if it is big enough, otherwise append a
    // new one. Chunks are never freed until release(), so a solve reaches steady
    // state after the first iteration.
    const size_t next = (chunk_index_ == kNoChunk) ? 0 : chunk_index_ + 1;
    if (next < chunks_.size() && chunks_[next].size >= min_bytes) {
        chunk_index_ = next;
        cursor_ = 0;
        return;
    }

    // Rust fails outright for requests over 1 MB; we give them a private chunk.
    const size_t size = std::max(kChunkSize, align_up(min_bytes));
    auto* base = static_cast<std::byte*>(::operator new(size, std::align_val_t{kAlignment}));

    if (next < chunks_.size()) {
        // The existing chunk at this slot is too small: replace it.
        ::operator delete(chunks_[next].base, std::align_val_t{kAlignment});
        chunks_[next] = Chunk{base, size};
    } else {
        chunks_.push_back(Chunk{base, size});
    }
    chunk_index_ = next;
    cursor_ = 0;
}

void* Arena::allocate_bytes(size_t bytes) {
    const size_t size = align_up(bytes);
    if (chunk_index_ == kNoChunk || cursor_ + size > chunks_[chunk_index_].size) grow(size);
    void* p = chunks_[chunk_index_].base + cursor_;
    cursor_ += size;
    return p;
}

}  // namespace pfs
