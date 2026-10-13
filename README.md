# postflop-solver-cpp

A C++20 port of [postflop-solver](https://github.com/b-inary/postflop-solver), an
open-source Texas hold'em postflop solver using Discounted CFR.

Original work Copyright (C) 2022 Wataru Inariba, licensed AGPL-3.0-or-later. This
port is a derivative work and carries the same license — see [LICENSE](LICENSE).
The Rust sources this was ported from are in `../postflop-solver-main`.

**Notice: Both the porting and extensions/changes to this repository is primarily done using VIBE coding.**

## Building

Requires a C++20 compiler and CMake 3.24+. No external dependencies in the
default configuration.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

On Windows with the Visual Studio generator:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
.\build\tests\Release\pfs_tests.exe
```

### Options

| Option | Default | Rust equivalent |
|---|---|---|
| `PFS_ENABLE_THREADS` | ON | `rayon` |
| `PFS_ENABLE_SERIALIZATION` | ON | `bincode` |
| `PFS_CUSTOM_ALLOC` | OFF | `custom-alloc` |
| `PFS_ENABLE_ZSTD` | OFF | `zstd` |
| `PFS_ARCH` | `""` | — (`AVX2` to widen the kernels) |

`PFS_CUSTOM_ALLOC` switches the solver's per-node temporaries to a thread-local
bump arena. It is off by default so the pinned reference numbers are produced by
the ordinary allocator; the test suite passes identically either way.

### Floating point

**Do not build with `/fp:fast`, `-ffast-math` or `-Ofast`.** CMake rejects them at
configure time and `src/core/numeric.hpp` `#error`s on `__FAST_MATH__`. The solver
depends on IEEE semantics in four places:

1. `is_zero(x)` is `bit_cast<uint32_t>(x) == 0`, so `-0.0` must not compare as zero.
2. Node locking encodes "unlocked" as `-1.0` and "locked to zero frequency" as
   `+0.0`, and the DCFR update branches on the sign of a regret that can be `-0.0`.
   Signed zero is load-bearing.
3. `index_to_card_pair` inverts a triangular number with `sqrt`/`ceil` and indexes
   a 1326-entry table; an off-by-one there is a silently wrong solve.
4. Every reduction is deliberately ordered with explicit `double` accumulators.

FMA contraction is also disabled (`-ffp-contract=off`, and
`#pragma fp_contract(off)` in the kernels, since MSVC has no `/fp:contract-`
switch). Under `/arch:AVX2` MSVC would otherwise fuse `d += s1 * s2` into an FMA,
which Rust/LLVM does not, changing the low bits of every result.

## Usage

See [examples/basic.cpp](examples/basic.cpp) for the full flow. In outline:

```cpp
#include <pfs/game.hpp>
#include <pfs/solver.hpp>

pfs::CardConfig card_config;
card_config.range[0] = pfs::Range::parse("66+,A8s+,AJo+,K9s+,KQo").value();
card_config.range[1] = pfs::Range::parse("QQ-22,AQs-A2s,ATo+").value();
card_config.flop = pfs::flop_from_str("Td9d6h").value();
card_config.turn = pfs::card_from_str("Qc").value();

pfs::TreeConfig tree_config;
tree_config.initial_state = pfs::BoardState::Turn;  // must match card_config
tree_config.starting_pot = 200;
tree_config.effective_stack = 900;
const auto sizes = pfs::BetSizeOptions::parse("60%, e, a", "2.5x").value();
tree_config.turn_bet_sizes = {sizes, sizes};
tree_config.river_bet_sizes = {sizes, sizes};
tree_config.risk_premium = {0.0, 0.0};   // chip EV; see "Risk premium" below

auto tree = pfs::ActionTree::create(std::move(tree_config)).value();
auto game = pfs::PostFlopGame::with_config(std::move(card_config), std::move(tree)).value();

game->allocate_memory(false);            // true for the 16-bit mode
pfs::solve(*game, 1000, 1.0f, true);

game->cache_normalized_weights();
const auto ev = game->expected_values(0); // 0 = OOP
```

### Risk premium

By default the solver maximises chip EV, so it takes any line with EV above zero. In a tournament
losing chips costs more equity than winning the same chips gains, so a line has to clear a higher
bar. `TreeConfig::risk_premium` gives each player that bar as an ICM **risk premium**: the extra
equity they need before stacking off, as a fraction in `[0, 0.5)`. A premium of `0.02` means the
player needs 52% rather than 50% to call an all-in for stacks.

Internally the premium becomes a **bubble factor**, the ratio of equity lost when losing to equity
gained when winning:

```
BF = (1 + 2 * premium) / (1 - 2 * premium)      0.02 -> 1.0833    0.10 -> 1.5
```

A player's losing payoffs are weighted by their bubble factor; winning and chopping keep their chip
value. Equity is unaffected. The premium is a fixed linearisation of ICM, not an ICM
implementation: there is no payout ladder and no stack-dependent recalculation.

Four consequences:

- **The game is no longer zero-sum** — as with rake, which is why the machinery already exists.
  `is_zero_sum()` returns false and `compute_exploitability` measures each player's gain from
  deviating rather than summing the best responses. Unlike rake, the asymmetry is between the
  players, and two-player Discounted CFR is only guaranteed to converge to a Nash equilibrium in
  zero-sum games, so the reported number can plateau above the target. On the example game in
  `simulations/` a 2% premium converges indistinguishably from chip EV; see `STATUS.md`.
- **The reported EVs are risk-adjusted, not chips.** `ev_oop + ev_ip` no longer equals the pot, and
  a fold is no longer worth 0: it forfeits the chips already committed, which the premium weights.
- **Only postflop chips are priced.** The preflop investment sits in `starting_pot` and is treated
  as sunk, so the bubble factor should be calibrated at the stack each player has when the postflop
  hand begins. A large pot against short stacks shows little risk aversion because little is left
  to lose.
- **16-bit mode loses precision.** Values are quantised against the largest magnitude in a slice,
  which a premium inflates, so `allocate_memory(true)` is a poor fit for large premiums.

A premium cannot be combined with a rake large enough to exceed the starting pot, because a chopped
pot would then be a net loss and the model only weights the losing outcome. `ActionTree::create`
rejects that combination.

The solver also works with any type satisfying the `pfs::GameLike` concept, not
just `PostFlopGame`. [tests/test_kuhn.cpp](tests/test_kuhn.cpp) and
[tests/test_leduc.cpp](tests/test_leduc.cpp) implement the interface from outside
the library:

```cpp
class MyGame : public pfs::GameBase<MyGame, MyNode> { /* ... */ };
static_assert(pfs::GameLike<MyGame>);
```

### Long-running tests

The exhaustive 7-card evaluation, the C(49,6) combinadic sweep and the two
PioSOLVER-verified presets are skipped by default:

```sh
PFS_RUN_SLOW=1 ./build/tests/pfs_tests
```

## Command line solver

`solver/` builds `postflop-solver`, which solves a game exported in JSON format by tools
in the separate poker-tools repository. It writes the first street as json, the complete
solved tree as a solution file, or both:

```sh
postflop-solver game.json [--output result.json] [--solution result.pfsol] [--ranges ranges.json]
                [--iterations 1000] [--exploitability 0.25] [--compress]
                [--risk-premium <oop> <ip>] [--quiet]
```

`--ranges` solves the game tree with the ranges of a range file (`poker-tools/range`,
e.g. preflop ranges exported by poker-tools' hrc-analyzer) instead of the game file's
ranges. It warns when the range file's stack-to-pot ratio differs from the game's, if the
range file has the stacks and pot.

Instead of a game file, a game can be put together from two files and a board: a game tree
file (`poker-tools/tree`, written by poker-tools' game-tree-builder) and a range file. The
board is given on the command line, as the flop and, if the tree starts on the turn or
river, those cards: `KcQdJs`, `KcQdJsTh` or `KcQdJsTh2c`.

```sh
postflop-solver --tree tree.json --ranges ranges.json --board KcQdJs [--output result.json] [--solution result.pfsol]
```

The result and solution files then have an empty `source` and `start`, and `input` is the
tree file.

`--exploitability` is the target as a percentage of the starting pot, and `--compress`
stores the solver data as 16-bit integers. The input format is described in
`solver/src/game_file.hpp` and the output format in `solver/src/result_file.hpp`. The
result has the strategy, EVs and equity per hand for every node of the first street.

`--risk-premium <oop> <ip>` takes both players' ICM risk premiums as percentages, since they
need not be the same — `--risk-premium 2 5` gives OOP 2% and IP 5%, and `--risk-premium 0 0`
is the chip-EV default. See [Risk premium](#risk-premium) for what the number means and what
it changes. The game and range files may carry their own `riskPremiumPercent`, which the
command line overrides:

```json
"riskPremiumPercent": { "OOP": 2.0, "IP": 5.0 }
```

It belongs in the files that carry ranges, because the preflop ranges are normally solved
under the same premiums; a range file's premiums replace the game file's just as its ranges
do. Both the result and the solution file always report `riskPremiumPercent` and the derived
`bubbleFactor`, so a reader never has to guess whether the EVs are chips or risk-adjusted.

`--solution` writes every street and every dealt card to a binary file, specified in
[documentation/SolverFileFormat.md](documentation/SolverFileFormat.md). It is meant for
other applications to browse. A reader loads one node at a time with file seeks or HTTP
range requests, so the file never has to fit in memory. Options:

| Option | Default | Meaning |
|---|---|---|
| `--solution-streets flop\|turn\|river` | `river` | The last street stored |
| `--solution-precision u16\|f32` | `u16` | 16-bit values, or the exact 32-bit floats |
| `--solution-codec zlib\|none` | `zlib` | Compression of each node |
| `--solution-prune <percent>` | 0 (off) | Leave out what follows an action the range takes less often than this |
| `--solution-prune-line <percent>` | 0 (off) | Leave out lines played less often than this |

For the example game `simulations/input/KcQdJs_basic_game.json` (132 and 103 combos, 52,684
nodes), the solution file is 60 MB and takes 4 seconds to write. It is 46 MB with
`--solution-prune-line 1`, 25 MB with `--solution-prune-line 5`, and 1.4 MB with
`--solution-streets turn`. `solver/src/solution_reader.hpp` is a C++ reader.

nlohmann/json, which it uses for parsing, is included in `third-party/nlohmann` (MIT), and
miniz, which compresses the solution file, in `third-party/miniz` (MIT). It is built when
this is the top-level project (`PFS_BUILD_SOLVER`).

## Implementation notes

- **Algorithm**: Discounted CFR with γ = 3.0 (not the paper's 2.0), and the
  cumulative strategy is reset whenever the iteration count reaches a power of 4.
- **Precision**: 32-bit floats throughout, with 64-bit accumulation for every
  summation. An optional mode stores each node's values as 16-bit integers with a
  single float scale.
- **Parallelism**: a hand-written work-stealing pool replaces rayon. The solver
  parallelizes over a node's child actions at every flop and turn node, so
  parallel regions nest 10–25 deep; `parallel_for` therefore never idles, it
  executes queued work until its own job completes. See the comment at the top of
  `src/core/thread_pool.hpp`.
- **Determinism**: results are bit-identical for any thread count. Children write
  disjoint rows and every reduction runs sequentially in fixed row order.
