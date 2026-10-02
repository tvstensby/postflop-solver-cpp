// Port of src/hand_table.rs. Internal (Rust: pub(crate)).
#pragma once

#include <pfs/common.hpp>

#include <array>

namespace pfs {

// The number of distinct 7-card hand equivalence classes in Texas hold'em.
inline constexpr size_t kHandTableSize = 4824;

// Ascending; see src/hand_table.cpp for provenance.
extern const std::array<int32_t, kHandTableSize> kHandTable;

}  // namespace pfs
