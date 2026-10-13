# Port status

The port is **complete**. This file records what each piece was validated against
and every place the C++ deliberately differs from the Rust.

## Coverage

| # | Milestone | Rust source | Validated by |
|---|---|---|---|
| 1 | Core kernels, thread pool, arena | `sliceop.rs`, `atomic_float.rs`, `alloc.rs`, `mutex_like.rs`, part of `utility.rs` | `test_core.cpp` — 18 tests incl. 3-level nested `parallel_for` |
| 2 | Cards and hand evaluation | `card.rs`, `hand.rs`, `hand_table.rs` | `test_card.cpp` — exhaustive C(52,7) = 133,784,560 hands, all 4824 table entries reached, all 9 category counts exact |
| 3 | Range parsing and formatting | `range.rs` | `test_range.cpp` — the crate's own scanner, trim, `range_from_str` and 10 golden `to_string` tables |
| 4 | Bet sizes and action tree | `bet_size.rs`, `action_tree.rs` | `test_bet_size.cpp` (incl. the 27-case error table), `test_action_tree.cpp` |
| 5 | Solver core, DCFR, exploitability | `solver.rs`, `utility.rs`, `interface.rs` | `test_kuhn.cpp` (EV = −1/18), `test_leduc.cpp` (EV = −0.0856, plain and compressed) |
| 6a | `CardConfig`: enumeration, hand strength, isomorphism | the `CardConfig` half of `card.rs` | `test_card_config.cpp` |
| 6b | `PostFlopGame` | `game/{mod,node,base,evaluation,interpreter}.rs` | `test_game.cpp` — the closed-form EV tables, all three rake variants, node locking, `remove_lines`; `test_pio_regression.cpp` — `isomorphism_monotone` |
| 6c | Risk premium (no Rust counterpart) | — | `test_risk_premium.cpp` — the closed-form weighted loss, per-player asymmetry, the charged fold row, loss of zero-sum, a tightening all-in calling range, EV/detail consistency, compressed agreement |
| 7 | Serialization and file I/O | `game/serialization.rs`, `file.rs` | `test_file_io.cpp` — River/Turn/Flop round-trips, bit-exact `equity(0)`, lock survival |
| 8 | Bunching effect | `bunching.rs` + the bunching half of `game/base.rs` | `test_bunching.cpp` (combinadics, exhaustive C(49,4) and C(49,5)), `test_bunching_game.cpp` (`[7.5, −7.5]`, `[30, −30]`) |
| 9 | Examples and PioSOLVER regressions | `examples/*.rs`, the two `#[ignore]`d tests | the three examples run as tests; `pio_slow.*` |

**121 tests pass in every configuration** — default, `PFS_ENABLE_THREADS=OFF`,
`PFS_CUSTOM_ALLOC=ON`, `PFS_ARCH=AVX2` — with zero compiler warnings.
`PFS_ENABLE_SERIALIZATION=OFF` runs 115 (the six `file_io` cases are excluded,
since `save/load` and the storage-mode API only exist with serialization on).
The `postflop-solver` application has 10 tests of its own (`postflop_solver_tests`).

Long-running cases are gated behind `PFS_RUN_SLOW=1`: the exhaustive 7-card
evaluation, the C(49,6) combinadic sweep, and the two PioSOLVER presets. The
examples carry the `slow` CTest label, so `ctest -LE slow` runs the unit suite
alone in about 8 seconds.

The three examples reproduce their Rust counterparts' assertions, including
`basic`'s exact action lists and its `KsJs` nut-straight check, and `file_io`'s
`11.50MB` original / `0.80MB` truncated memory report (the Rust prints 0.79MB; see
deviation 9).

## Deliberate deviations from the Rust

1. **`MutexLike` / `MutexGuardLike` are not ported.** They are a fake mutex — an
   `UnsafeCell` whose `lock()` never locks — existing only to hand out `&mut` from
   `&` against the borrow checker. `root()` and `play()` return `Node&` and every
   `.lock()` is gone. Consequently the `GameNode` accessors are non-const.
2. **`_mut` and `set_*` pairs are collapsed.** `strategy()` returns
   `std::span<float>`; `strategy_scale()` returns `float&`. This halves the
   interface with no semantic change.
3. **The arena enforces LIFO structurally.** Rust's `StackAlloc` panics at runtime
   if `deallocate` is not perfectly stack-ordered. `Arena::Scope` restores the
   saved watermark in its destructor instead, so the discipline cannot be violated
   by reordering locals. An oversized request also gets its own chunk rather than
   failing, so no caller has to reason about the 1 MB limit.
4. **`parallel_for_chunked` is a first-class API.** Rust's bunching phases use
   `.step_by(100)` semantically, not as a grain hint — the body unranks a
   combination once per chunk then Gosper-walks it — so the chunked form is
   explicit rather than an incidental iterator adaptor.
5. **Card/string helpers live in `card.cpp`**, and `CardConfig` in
   `card_config.hpp`. The Rust keeps the string helpers in `range.rs` and
   `CardConfig` in `card.rs`; splitting them breaks a header cycle.
6. **`std::regex` is not used.** It cannot express the `(?P<name>)` named groups
   the crate's range regex relies on, and it is slow. `src/range.cpp` has a
   hand-written scanner for the same grammar, pinned by the crate's own tables.
7. **`COMB_TABLE` is generated, not transcribed** — it is just C(n, k+1), so a
   `constexpr` function produces it. `HAND_TABLE` *is* transcribed verbatim, by
   `tools/gen_hand_table.py`, because `Hand::evaluate()` returns the index into it
   and that ranking is depended on everywhere downstream.
8. **Errors are `Result<T>` / `Status`; misuse throws.** This mirrors the Rust
   split, where construction and parsing return `Result<_, String>` while the
   interpreter API panics. Throwing rather than aborting lets the ported tests
   observe the failure modes.
9. **`misc_memory_usage` is an estimate and differs slightly.** It sums
   `sizeof(Self)` plus container capacities, which are not the same in C++ as in
   Rust. The storage arrays — which dominate — match exactly, so `memory_usage()`
   agrees to the reported precision but the last digit of the truncated figure can
   differ (0.80MB vs the Rust's 0.79MB).
10. **The serialization format is our own**, with a distinct magic number
    (`"PFSC"`) and fixed-width little-endian integers instead of bincode varints.
    A file written by the Rust solver is rejected cleanly rather than mis-parsed.
    The per-node record carries an explicit `has_storage` flag instead of relying
    on the Rust's implicit "storage1 is non-null" invariant, and the storage base
    pointers are threaded through the writer rather than through `thread_local`
    `Cell`s.
11. **Bunching result tables are plain floats.** Rust makes `result4/5/6` atomic
    only to get interior mutability through `&self`; each destination is written
    exactly once by exactly one task, so the atomics buy nothing. The genuinely
    contended tables (`sum_`, `temp_table3_`) do use `AtomicF64`.
12. **No `VirtualAlloc`/`mmap` for the 3.6 GB bunching table.** Four folded players
    need a 450,978,066-entry `AtomicF64` table, and `std::vector` value-initializes
    it, so that path commits the memory up front instead of relying on demand-zero
    paging. The Rust marks its own 4-player tests `#[ignore]` for the same cost
    reasons; 1–3 players need at most ~112 MB and are covered by the tests.
13. **A per-player ICM risk premium**, which the Rust has no concept of.
    `TreeConfig::risk_premium` weights a player's losing payoffs by their bubble factor,
    `(1 + 2r) / (1 - 2r)`, in the one place terminal payoffs are produced
    (`evaluate_internal` and `evaluate_internal_bunching`). Three things follow:
    - The game is no longer zero-sum, so `GameBase` gained `is_zero_sum()` (defaulted to
      `true`, and in the `GameLike` concept) and `compute_exploitability` gates on it instead
      of `is_raked()`. The non-zero-sum branch itself is the Rust's, unchanged — rake already
      needed it. An out-of-tree game that implements `GameLike` without deriving from
      `GameBase` now has to supply the method.
    - `expected_values_detail` reports a fold as 0 only when the player has no premium. With
      one, the row carries the premium on the chips already committed, and zeroing it would
      hide the modelled cost and disagree with `compute_current_ev`.
    - `kFileFormatVersion` is 2: the two premiums are written in `write_tree_config`, because
      loading regenerates counterfactual values from the saved strategy and would otherwise
      read a risk-averse solve as chip EV.

    The default is 0, and at 0 the payoff expression is bit-identical: `starting_pot` is
    validated positive so `half_pot` is strictly positive, `node.amount()` is non-negative,
    and `half_pot + 0.0 * amount == half_pot` exactly. The pinned numbers in `test_game.cpp`,
    `test_file_io.cpp` and `test_pio_regression.cpp` are unchanged, and the result JSON and
    solution-file node data for `simulations/input/KcQdJs_basic_game.json` compare equal to a
    pre-change build. The deliberately narrow form is also why the premium is applied only to
    `amount_lose`: the general `centered + extra * min(net, 0)` would add `+0.0` to
    `amount_tie`, which is `-0.0` in an unraked bunching solve, and the sign of that zero is
    load-bearing for `apply_locking_strategy` and the `normalized_strategy` fallback. A rake
    that can exceed the starting pot is rejected alongside a premium instead, since that is
    the only way a chop or a win could be a net loss.

    Two-player Discounted CFR is only guaranteed to reach a Nash equilibrium in zero-sum
    games, so with a premium the measured exploitability is each player's average gain from
    deviating and need not fall to zero. Measured on
    `simulations/input/stack-42bb-pot-6.5bb.json` (see "Risk premium convergence" below) it
    still converges.

## Upstream quirks reproduced on purpose

- `ActionTree::add_line` looks an action up *before* rewriting a max-amount
  `Bet`/`Raise` into an `AllIn`, then inserts at the index it found. Adding
  `Bet(900)` to a node that already offers `AllIn(900)` therefore succeeds and
  leaves a duplicate. Pinned by
  `action_tree.max_amount_bet_is_promoted_to_allin_without_researching`.
- `GameNode::player()` returns the raw flag byte, not a masked 0/1. The solver's
  `node.player() == player` test is only correct because `is_terminal()` and
  `is_chance()` are checked first. Do not "clean this up".
- `regrets()` and `cfvalues()` intentionally alias the same storage (regrets while
  solving, cfvalues after `finalize`), and so do `strategy()` and
  `cfvalues_chance()`.
- Calling `allocate_memory()` a second time downgrades `Solved` back to
  `MemoryAllocated`. That is the sanctioned way to re-solve after changing a node
  lock, and both the node-locking test and example depend on it.
- The three tree walks (`solve_recursive`, `compute_cfvalue_recursive`,
  `compute_best_cfv_recursive`) had already drifted from each other in three
  observable ways in the original. The drift is preserved; see the table at the top
  of `include/pfs/utility.hpp`.
- Bunching phases 1 and 2 accumulate through `AtomicF64::add`, so they are not
  bitwise reproducible across runs. The tests that assert exact values use
  configurations whose results are integers below 2^53, where reordering is exact.

## Risk premium convergence

A risk premium makes the game general-sum, where Discounted CFR self-play has no convergence
guarantee, so this was measured rather than assumed.
`simulations/input/stack-42bb-pot-6.5bb.json` with `--ranges mp_raise_lp_call.json --board
KcQdJs`, both players on the same premium, 100 iterations with no early stop. The figures are
exploitability as a percentage of the starting pot, which for a premium is each player's average
gain from deviating:

| Premium | iter 10 | 30 | 50 | 70 | 100 |
|---|---|---|---|---|---|
| 0% (chip EV) | 49.6% | 13.2% | 5.30% | 5.93% | 1.67% |
| 2% | 55.1% | 13.9% | 5.95% | 5.75% | 1.76% |
| 5% | 58.1% | 15.6% | 6.84% | 6.80% | 2.02% |
| 10% | 78.3% | 18.7% | 7.55% | 6.59% | 1.89% |

So on this game a premium costs a little early progress and lands in the same place. Note that the
dip and rise between iterations 50 and 70 is present at 0% as well — that is the discounting, not
the premium. Two caveats stand regardless: nothing here proves convergence for a different tree or
a larger premium, and each exploitability check runs four tree walks instead of two, since the
current strategy's values no longer cancel.

## Two findings worth recording

**FMA contraction.** MSVC under `/arch:AVX2` fuses `d += s1 * s2` in
`fma_slices_into` into a single FMA; Rust/LLVM does not, because rustc never sets
the `contract` fast-math flag. That silently changes the low bits of every result.
MSVC has no `/fp:contract-` switch (it is accepted and ignored), so the kernels
carry `#pragma fp_contract(off)` and CMake rejects fast-math flags outright.

**The bunching normalizer.** `expected_values_detail` must multiply back by
`bunching_num_combinations` when bunching is active, not `num_combinations` — the
terminal evaluation divided by the former. Getting this wrong leaves every EV at
exactly half the pot, because the cfvalue term scales to nothing while the pot bias
survives. `compute_current_ev` and `equity` are unaffected, so only the
`expected_values` assertions catch it.
