// Port of src/interface.rs -- the Game / GameNode abstraction the solver is
// written against.
//
// Rust uses two traits with an associated type and ~36 defaulted methods, and
// `solve<T: Game>` is monomorphized. Virtual dispatch would be unacceptable in
// the CFR inner loop (10^7-10^8 node visits per solve, and it would block
// inlining play() and strategy()), so this is CRTP: the base classes supply the
// defaults, a same-named member in the derived class hides them, and C++20
// concepts constrain the free function templates so a missing method produces one
// readable diagnostic instead of a template dump.
//
// Rust's MutexLike is a fake mutex -- an UnsafeCell whose lock() never locks --
// existing only to hand out `&mut` from `&`. It has no C++ equivalent and is not
// ported: root() and play() simply return `Node&`, and every `.lock()` is gone.
// Consequently the accessors here are non-const.
//
// Two aliasing facts to preserve: regrets() and cfvalues() intentionally return
// the SAME storage (regrets while solving, cfvalues after finalize), and so do
// strategy() and cfvalues_chance().
#pragma once

#include <pfs/card.hpp>
#include <pfs/common.hpp>

#include <concepts>
#include <cstdint>
#include <optional>
#include <span>

namespace pfs {

// Supplies the defaulted GameNode methods. Anything that Rust defaults to
// `unreachable!()` throws here; reaching it means an implementation advertised a
// capability it does not provide.
template <class Derived>
class GameNodeBase {
public:
    bool has_cfvalues_ip() { return false; }
    std::optional<size_t> cfvalue_storage_player() { return std::nullopt; }
    bool enable_parallelization() { return false; }

    std::span<float> cfvalues_ip() { PFS_UNREACHABLE(); }
    std::span<float> cfvalues_chance() { PFS_UNREACHABLE(); }

    // 16-bit compressed views. Strategy is unsigned, regrets and cfvalues signed.
    std::span<uint16_t> strategy_compressed() { PFS_UNREACHABLE(); }
    std::span<int16_t> regrets_compressed() { PFS_UNREACHABLE(); }
    std::span<int16_t> cfvalues_compressed() { PFS_UNREACHABLE(); }
    std::span<int16_t> cfvalues_ip_compressed() { PFS_UNREACHABLE(); }
    std::span<int16_t> cfvalues_chance_compressed() { PFS_UNREACHABLE(); }

    // Rust has getter/setter pairs; a reference accessor collapses them.
    float& strategy_scale() { PFS_UNREACHABLE(); }
    float& regret_scale() { PFS_UNREACHABLE(); }
    float& cfvalue_scale() { PFS_UNREACHABLE(); }
    float& cfvalue_ip_scale() { PFS_UNREACHABLE(); }
    float& cfvalue_chance_scale() { PFS_UNREACHABLE(); }
};

// Supplies the defaulted Game methods. NodeT is Rust's associated `type Node`.
template <class Derived, class NodeT>
class GameBase {
public:
    using Node = NodeT;

    bool is_ready() { return true; }
    bool is_raked() { return false; }
    bool is_compression_enabled() { return false; }
    std::span<const uint8_t> isomorphic_chances(const Node&) { return {}; }
    const SwapList& isomorphic_swap(const Node&, size_t) { PFS_UNREACHABLE(); }
    std::span<const float> locking_strategy(const Node&) { return {}; }
};

template <class N>
concept GameNodeLike = requires(N& n, size_t a) {
    { n.is_terminal() } -> std::convertible_to<bool>;
    { n.is_chance() } -> std::convertible_to<bool>;
    // NOTE: this is the RAW player byte including the terminal/chance/fold flags,
    // not a masked 0/1. The solver's `node.player() == player` test is only
    // correct because is_terminal() and is_chance() are checked first.
    { n.player() } -> std::convertible_to<size_t>;
    { n.num_actions() } -> std::convertible_to<size_t>;
    { n.play(a) } -> std::same_as<N&>;
    { n.strategy() } -> std::same_as<std::span<float>>;
    { n.regrets() } -> std::same_as<std::span<float>>;
    { n.cfvalues() } -> std::same_as<std::span<float>>;
    { n.enable_parallelization() } -> std::convertible_to<bool>;
};

template <class G>
concept GameLike = GameNodeLike<typename G::Node> &&
    requires(G& g, size_t p, typename G::Node& n, std::span<float> res,
             std::span<const float> cfr) {
    { g.root() } -> std::same_as<typename G::Node&>;
    { g.num_private_hands(p) } -> std::convertible_to<size_t>;
    { g.initial_weights(p) } -> std::same_as<std::span<const float>>;
    // `res` is uninitialized on entry; the implementation must write every element.
    g.evaluate(res, n, p, cfr);
    { g.chance_factor(n) } -> std::convertible_to<size_t>;
    { g.is_solved() } -> std::convertible_to<bool>;
    g.set_solved();
    { g.is_ready() } -> std::convertible_to<bool>;
    { g.is_raked() } -> std::convertible_to<bool>;
    { g.is_compression_enabled() } -> std::convertible_to<bool>;
    { g.isomorphic_chances(n) } -> std::same_as<std::span<const uint8_t>>;
    { g.locking_strategy(n) } -> std::same_as<std::span<const float>>;
};

}  // namespace pfs
