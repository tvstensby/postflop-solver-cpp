// Parses ranges in GTO+ syntax, as written by poker-tools' gto-pluss-exporter:
//
//   AA-22,AKs-ATs,KQs-KJs,QJs,AKo-AJo,KQo,[50.0]JTs,KJo[/50.0],[12.5]AhKh[/12.5]
//
//   - comma-separated elements: pairs (AA), suited and offsuit classes (AKs, AKo),
//     both (AK), single combos (AhKh), dash runs (AA-22, AKs-ATs) and plus
//     forms (TT+, A9s+)
//   - "[w]" before an element starts a group with weight w percent, and "[/w]"
//     after an element ends it; elements outside groups have weight 100%
//   - where elements overlap, the later one wins
#pragma once

#include <pfs/range.hpp>
#include <pfs/result.hpp>

#include <string>

namespace pfs_solver {

pfs::Result<pfs::Range> parse_gto_range(const std::string& text);

}  // namespace pfs_solver
