# postflop-solver-cpp

A C++20 port of the Rust [postflop-solver](https://github.com/b-inary/postflop-solver) (Discounted
CFR). AGPL-3.0-or-later, see `LICENSE`. `README.md` covers usage and options, and `STATUS.md`
covers what was validated and every deliberate deviation from the Rust code.

## Related repository: poker-tools

`poker-tools` is a separate repository with poker utilities: a range combiner, an HRC export
analyzer and a GTO+ file exporter. Both repositories are assumed to be cloned next to each other,
together with the Rust sources this was ported from:

```
<workspace>/
  postflop-solver-cpp/   this repository (AGPL-3.0-or-later)
  poker-tools/           MIT
  postflop-solver-main/  the Rust original, the reference for the port
```

Refer to the others as `../poker-tools` and `../postflop-solver-main`. The repositories are kept
separate because their licenses differ:

- Do not copy code from this repository into `poker-tools`.
- Do not make `poker-tools` build or link this library without asking first. That would put the
  linking program under the AGPL.

## Building

A C++20 compiler and CMake 3.24 or later. No external dependencies in the default configuration.

On Windows with Visual Studio 2022, into `build-vscode/` (the folder VS Code uses):

```sh
cmake -S . -B build-vscode -G "Visual Studio 17 2022" -A x64
cmake --build build-vscode --config Release
ctest --test-dir build-vscode -C Release -LE slow --output-on-failure
```

Elsewhere:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build -LE slow --output-on-failure
```

- `ctest -LE slow` runs the unit suite (`pfs_tests`) in about 8 seconds. Without `-LE slow` the
  three examples also run as tests, and each can take up to 30 minutes.
- `PFS_RUN_SLOW=1` enables the long-running unit cases (exhaustive 7-card evaluation, the
  PioSOLVER presets).
- Tests, examples and the solver application are built when this is the top-level project
  (`PFS_BUILD_TESTS`, `PFS_BUILD_EXAMPLES`, `PFS_BUILD_SOLVER`). The other options are listed in
  `README.md`.
- 64-bit targets only.

## The postflop-solver application

`solver/` contains `postflop-solver` (`postflop-solver.exe`), which solves games exported using
JSON exported by tools in `../poker-tools`:

```sh
build-vscode/solver/Release/postflop-solver.exe game.json --output result.json
```

- The two repositories exchange data only through files, mostly json: the input game file
  described in `solver/src/game_file.hpp`) and the result described in `solver/src/result_file.hpp`).
  When either format changes, update the other repository's reader or writer too and bump the format version.
- Both repositories build on their own. `third-party/nlohmann` is a copy of the one in
  `../poker-tools/third-party/nlohmann`. Keep the copies at the same version.
- Amounts in both files are integers: GTO+ chips times `chipScale`.

## Rules that keep results correct

- Never build with `/fp:fast`, `-ffast-math` or `-Ofast`, and keep FMA contraction off. The solver
  relies on signed zeros and on the order of its reductions. CMake and `src/core/numeric.hpp`
  reject fast-math. See "Floating point" in `README.md`.
- Results must stay bit-identical to the Rust reference and for any thread count. Changes to
  kernels or reductions must keep the pinned test numbers passing. Don't update expected values to
  make a test pass.
- The code builds without warnings (`/W4` on MSVC; `-Wall -Wextra -Wpedantic` and more elsewhere).
