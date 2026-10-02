#include "result_file.hpp"

#include <algorithm>
#include <cmath>
#include <span>

using nlohmann::ordered_json;

namespace pfs_solver {

namespace {

const char* const kPlayerNames[2] = {"OOP", "IP"};

double rounded(double value, int decimals) {
    const double scale = std::pow(10.0, decimals);
    return std::round(value * scale) / scale;
}

std::string hand_name(const pfs::Hole& hole) {
    // Card ids are 4 * rank + suit, and holes are stored low card first.
    return pfs::card_to_string(hole.second).value() + pfs::card_to_string(hole.first).value();
}

double weighted_average(std::span<const float> values, std::span<const float> weights) {
    double sum = 0.0;
    double total = 0.0;
    for (size_t i = 0; i < values.size(); ++i) {
        sum += static_cast<double>(values[i]) * weights[i];
        total += weights[i];
    }
    return total > 0.0 ? sum / total : 0.0;
}

// Plays the history from the root. The nodes on the first street need no chance
// actions, so every action of the history is a player action.
bool go_to(pfs::PostFlopGame& game, const std::vector<pfs::Action>& history) {
    game.back_to_root();
    for (const pfs::Action& action : history) {
        const std::vector<pfs::Action> actions = game.available_actions();
        auto it = std::find(actions.begin(), actions.end(), action);
        if (it == actions.end() || game.is_chance_node()) return false;
        game.play(static_cast<size_t>(it - actions.begin()));
    }
    return true;
}

ordered_json node_result(pfs::PostFlopGame& game, const GameFileNode& node) {
    game.cache_normalized_weights();
    const size_t player = game.current_player();
    const std::vector<pfs::Action> actions = game.available_actions();
    const std::vector<float> strategy = game.strategy();
    const std::vector<float> evs = game.expected_values_detail(player);
    const std::vector<float> equity = game.equity(player);
    // Reach weights for the hands, normalized weights (which also account for the
    // opponent hands each hand blocks) for the range averages.
    const std::span<const float> reach = game.weights(player);
    const std::span<const float> weights = game.normalized_weights(player);
    const std::span<const pfs::Hole> hands = game.private_cards(player);
    const size_t num_hands = hands.size();

    // The game's action index for each action of the file, in the file's order.
    std::vector<size_t> order;
    for (const std::string& name : node.actions) {
        size_t index = 0;
        while (index < actions.size() && actions[index].to_string() != name) ++index;
        order.push_back(index);
    }

    ordered_json result;
    result["id"] = node.id;
    result["player"] = kPlayerNames[player];
    result["history"] = ordered_json::array();
    for (const pfs::Action& action : node.history) result["history"].push_back(action.to_string());
    result["actions"] = node.actions;

    std::vector<double> frequencies(order.size(), 0.0);
    double total_weight = 0.0;
    ordered_json hands_json = ordered_json::array();
    for (size_t h = 0; h < num_hands; ++h) {
        if (reach[h] <= 0.0f) continue;
        ordered_json hand;
        hand["hand"] = hand_name(hands[h]);
        hand["weight"] = rounded(reach[h], 6);
        ordered_json hand_frequencies = ordered_json::array();
        ordered_json hand_evs = ordered_json::array();
        for (size_t a = 0; a < order.size(); ++a) {
            const size_t i = order[a] * num_hands + h;
            hand_frequencies.push_back(rounded(strategy[i], 6));
            hand_evs.push_back(rounded(evs[i], 4));
            frequencies[a] += static_cast<double>(weights[h]) * strategy[i];
        }
        total_weight += weights[h];
        hand["frequencies"] = hand_frequencies;
        hand["evs"] = hand_evs;
        hand["equity"] = rounded(equity[h], 6);
        hands_json.push_back(hand);
    }

    ordered_json range_frequencies = ordered_json::array();
    for (double frequency : frequencies)
        range_frequencies.push_back(rounded(total_weight > 0.0 ? frequency / total_weight : 0.0, 6));
    result["rangeFrequencies"] = range_frequencies;
    result["hands"] = hands_json;
    return result;
}

}  // namespace

ordered_json build_result(pfs::PostFlopGame& game, const GameFile& file,
                          const std::string& input_name, const SolveSettings& settings,
                          const SolveStats& stats) {
    ordered_json out;
    out["format"] = "postflop-solver/result";
    out["version"] = 1;
    out["input"] = input_name;
    out["source"] = file.source;
    out["start"] = file.start_id;
    out["chipScale"] = file.chip_scale;
    out["board"] = {{"flop", file.flop},
                    {"turn", file.turn.empty() ? ordered_json(nullptr) : ordered_json(file.turn)},
                    {"river", file.river.empty() ? ordered_json(nullptr) : ordered_json(file.river)}};

    out["settings"] = {{"maxIterations", settings.max_iterations},
                       {"targetExploitabilityPercent", settings.target_exploitability_percent},
                       {"compression", settings.compression},
                       {"threads", stats.threads}};

    const double pot = file.tree_config.starting_pot;
    out["solve"] = {{"iterations", stats.iterations},
                    {"exploitability", rounded(stats.exploitability, 4)},
                    {"exploitabilityPercent", rounded(100.0 * stats.exploitability / pot, 4)},
                    {"seconds", rounded(stats.seconds, 3)},
                    {"memoryMB", rounded(static_cast<double>(stats.memory_bytes) / (1024.0 * 1024.0), 1)}};

    game.back_to_root();
    game.cache_normalized_weights();
    ordered_json ev;
    ordered_json equity;
    for (size_t player = 0; player < 2; ++player) {
        const std::span<const float> weights = game.normalized_weights(player);
        ev[kPlayerNames[player]] = rounded(weighted_average(game.expected_values(player), weights), 4);
        equity[kPlayerNames[player]] = rounded(weighted_average(game.equity(player), weights), 6);
    }
    out["ev"] = ev;
    out["equity"] = equity;

    const char* first_street = file.tree_config.initial_state == pfs::BoardState::Flop   ? "flop"
                               : file.tree_config.initial_state == pfs::BoardState::Turn ? "turn"
                                                                                         : "river";
    ordered_json nodes = ordered_json::array();
    for (const GameFileNode& node : file.nodes) {
        if (node.street != first_street || !go_to(game, node.history)) continue;
        nodes.push_back(node_result(game, node));
    }
    out["nodes"] = nodes;
    game.back_to_root();
    return out;
}

}  // namespace pfs_solver
