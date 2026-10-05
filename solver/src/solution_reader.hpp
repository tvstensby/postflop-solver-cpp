// Reads solution files (format "postflop-solver/solution", version 1, see
// documentation/SolverFileFormat.md) one node at a time, seeking to the parts it needs. Used by
// the tests of the writer, and as a reference for readers in other languages.
#pragma once

#include "solution_format.hpp"

#include <pfs/card.hpp>
#include <pfs/result.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <span>
#include <vector>

namespace pfs_solver {

class SolutionReader {
public:
    enum class State { Stored, Pruned, NotStored, Impossible, Empty };

    // A node's data, translated into the actual suits of the board it was loaded for. Per-hand
    // arrays follow hands(player); strategy and action_ev are action-major ([a * H + h]).
    struct Node {
        State state = State::NotStored;
        size_t template_id = 0;
        uint8_t player = solution::kNoPlayer;  // the acting player at action nodes
        size_t num_actions = 0;
        std::vector<float> strategy;
        std::vector<float> action_ev;
        std::array<std::vector<float>, 2> weight;
        std::array<std::vector<float>, 2> normalized_weight;
        std::array<std::vector<float>, 2> ev;
        std::array<std::vector<float>, 2> equity;
        std::vector<float> summary;
    };

    static pfs::Result<std::unique_ptr<SolutionReader>> open(const std::filesystem::path& path);

    const nlohmann::json& metadata() const { return metadata_; }
    const std::vector<pfs::Hole>& hands(size_t player) const { return hands_[player]; }

    // Loads an action or chance node of the template. cards are the actual cards dealt on the way
    // to the node, in order: none on the first street, then the turn and/or the river.
    pfs::Result<Node> load(size_t template_id, std::span<const pfs::Card> cards = {});

private:
    struct Segment {
        std::vector<solution::BlobRef> slots;
        std::vector<solution::SlotState> states;
        std::vector<solution::SegmentRef> exits;  // [exit * 52 + card]
    };

    struct TreeNode {
        bool terminal = false;
        size_t group = 0;
        size_t slot = 0;
        size_t exit = 0;
        bool has_parent = false;
        size_t parent = 0;
        uint8_t player = solution::kNoPlayer;
    };

    SolutionReader() = default;

    pfs::Result<std::vector<uint8_t>> read(uint64_t offset, uint64_t length);
    pfs::Result<std::vector<uint8_t>> read_blob(const solution::BlobRef& ref, solution::BlobType type);
    pfs::Result<const Segment*> segment(const solution::SegmentRef& ref);
    pfs::Result<std::vector<size_t>> hand_permutation(size_t player, uint8_t perm) const;

    std::ifstream stream_;
    uint64_t length_ = 0;
    bool crc_ = false;
    solution::SegmentRef root_;
    nlohmann::json metadata_;
    std::array<std::vector<pfs::Hole>, 2> hands_;
    std::array<std::vector<int32_t>, 2> hand_index_;  // pair index -> hand index, -1 if absent
    std::vector<TreeNode> tree_;
    std::vector<size_t> group_roots_;
    std::map<uint64_t, Segment> segments_;
};

}  // namespace pfs_solver
