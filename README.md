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

auto tree = pfs::ActionTree::create(std::move(tree_config)).value();
auto game = pfs::PostFlopGame::with_config(std::move(card_config), std::move(tree)).value();

game->allocate_memory(false);            // true for the 16-bit mode
pfs::solve(*game, 1000, 1.0f, true);

game->cache_normalized_weights();
const auto ev = game->expected_values(0); // 0 = OOP
```

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
in the separate poker-tools repository and writes the solution as json:

```sh
postflop-solver game.json --output result.json [--ranges ranges.json] [--iterations 1000] [--exploitability 0.25] [--compress] [--quiet]
```

`--ranges` solves the game tree with the ranges of a range file (`poker-tools/range`,
e.g. preflop ranges exported by poker-tools' hrc-analyzer) instead of the game file's
ranges. It warns when the range file's stack-to-pot ratio differs from the game's.

`--exploitability` is the target as a percentage of the starting pot, and `--compress`
stores the solver data as 16-bit integers. The input format is described in
`solver/src/game_file.hpp` and the output format in `solver/src/result_file.hpp`. The
result has the strategy, EVs and equity per hand for every node of the first street.
nlohmann/json, which it uses for parsing, is included in `third-party/nlohmann` (MIT).
It is built when this is the top-level project (`PFS_BUILD_SOLVER`).

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
