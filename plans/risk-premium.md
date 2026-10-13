# Per-player risk premium (ICM-style) in the solver

## Context

The solver maximises chip EV, so it takes any line with EV above zero. In a tournament, losing
chips costs more tournament equity than winning the same chips gains, so a line needs to clear a
higher bar. The standard ICM measure of that bar is the **risk premium**: the extra equity a
player needs before stacking off.

We want each player to carry one fixed risk premium for the whole solve — not a full ICM
implementation. A premium of 2% for OOP means OOP needs 52% equity, not 50%, to call an all-in
for stacks.

Research notes ([GTO Wizard: bubble factor](https://blog.gtowizard.com/what-is-the-bubble-factor-in-poker-tournaments/),
[GTO Wizard: risk premium](https://gtowizard.com/en/glossary/risk-premium/)): the risk premium is
derived from the **bubble factor** BF, the ratio of equity lost when losing to equity gained when
winning. Required equity for a fully-invested call is `BF / (BF + 1)`, so with
`RP = required_equity(ICM) - 0.5`:

```
BF = (1 + 2·RP) / (1 - 2·RP)        RP = 2%  ->  BF = 1.0833
BF - 1 = 4·RP / (1 - 2·RP)          RP = 5%  ->  BF = 1.2222
                                    RP = 10% ->  BF = 1.5
```

A fixed per-player bubble factor is the linearisation of ICM, which is exactly the "fixed premium,
not full ICM" the task asks for. [A public fork of postflop-solver](https://github.com/IrvinChoi/postflop)
does the same thing ("bubble-factor terminal payoff scaling") and reaches the same conclusions
about its consequences, which is a useful independent check on the design below.

## The model

Terminal payoffs in `src/game/evaluation.cpp:30-34` are **centred**: the stored value is
`net - starting_pot/2`, where `net` is the player's stack change from the start of the postflop
hand. With `a = node.amount()` (each player's *matched* postflop contribution) and
`half_pot = starting_pot/2 + a`:

| Outcome | `net` | centred amount today |
|---|---|---|
| win | `starting_pot + a - rake` | `amount_win = half_pot - rake` |
| tie | `starting_pot/2 - rake/2` | `amount_tie = -rake/2` |
| lose | `-a` | `amount_lose = -half_pot` |

Risk aversion multiplies a **negative** `net` by BF. Ignoring pathological rake (see Limitations),
losing is the only outcome with negative `net`, and there `net = -a`. So the whole feature is one
changed line per evaluator:

```cpp
amount_lose = -(half_pot + extra * node.amount()) / num_combinations_;   // extra = BF - 1 >= 0
```

`amount_win` and `amount_tie` are untouched. Two properties make this the right form:

- **It is state-dependent, so it actually changes the strategy.** A constant shift would cancel in
  the regret difference `cfv_a - Σ_b σ_b·cfv_b`. Scaling by `node.amount()` makes the penalty grow
  with chips risked, which is how rake gets its state dependence too.
- **It is bit-identical at premium 0.** `starting_pot > 0` is validated, so `half_pot > 0` strictly,
  `node.amount() >= 0`, and `half_pot + 0.0*a == half_pot` exactly. No signed-zero hazard.

Verification of the semantics — facing an all-in of `B` into pot `P` with `a` already matched,
indifference sits at `p = BF·B / (P + a + B + BF·(a + b))`. At `BF = 1` this is the pot-odds
`B/(P + 2a + 2B)`; fully invested (`P = a = 0`, `B = stack`) it is `BF/(1 + BF)`, i.e. exactly
`0.5 + RP`. The model reproduces the ICM definition in the case the definition is stated for.

**Do not** use the more general `centred + extra·min(net, 0)` form. It would add `+0.0` to
`amount_tie`, which is `-0.0` in an unraked bunching solve, flipping a sign bit that
`apply_locking_strategy` and the `normalized_strategy` fallback read
(`src/core/encoding.hpp:108-116`, `README.md:47-62`).

## Decisions

- **Scale**: a premium of 2 means 2 ICM equity points. Stored as a fraction in `TreeConfig`,
  entered as a percent on the CLI and in JSON.
- **The two players' premiums are independent**, so the CLI option takes both:
  `--risk-premium <oop> <ip>`, for example `--risk-premium 2 5`. Passing the same value twice is
  how you get a symmetric premium.
- **Reporting**: risk-adjusted EVs only. `ev_oop + ev_ip` no longer equals the pot, exactly as
  already happens with rake. Echo the premiums into the output so the numbers are interpretable.
- **Configuration**: CLI, plus an optional JSON key **in the files that carry ranges** — the
  `poker-tools/game` file and the `poker-tools/range` file — because the preflop ranges are
  normally solved under the same premiums. Not in `poker-tools/tree`, which has no ranges.
  Precedence: game file < range file < CLI.

## Implementation

### 1. Library: configuration

- `include/pfs/action_tree.hpp:84-85` — add after `rake_cap`, following the per-player
  `std::array<_, 2>` `[OOP, IP]` convention used by `flop_bet_sizes`:
  ```cpp
  std::array<double, 2> risk_premium{};  // ICM risk premium per player, in [0, 0.5)
  ```
- `src/action_tree.cpp:424-447` `check_config` — validate per player next to the `rake_rate`
  two-sided check: reject `< 0.0`, reject `>= 0.5` (BF would be infinite or negative), reject NaN
  (`!(x >= 0.0 && x < 0.5)` catches NaN in one test). A NaN premium would otherwise poison every
  terminal silently.
- Also reject a premium combined with a rake large enough to break the model: when any premium is
  non-zero, require `min((starting_pot + 2*effective_stack) * rake_rate, rake_cap) <= starting_pot`.
  See Limitations.

### 2. Library: payoffs

- `include/pfs/game.hpp` — two small accessors next to `is_raked()` (`:184-186`):
  ```cpp
  bool has_risk_premium() const noexcept {        // exact compare: a tolerance here would let a
      return tree_config_.risk_premium[0] != 0.0  // tiny premium change payoffs while still
          || tree_config_.risk_premium[1] != 0.0; // taking the zero-sum exploitability path
  }
  bool is_zero_sum() const noexcept { return !is_raked() && !has_risk_premium(); }
  // BF - 1 = 4r/(1-2r); the branch guarantees an exact +0.0 when off.
  double risk_loss_extra(size_t player) const noexcept {
      const double r = tree_config_.risk_premium[player];
      return r == 0.0 ? 0.0 : 4.0 * r / (1.0 - 2.0 * r);
  }
  ```
- `src/game/evaluation.cpp:34` and `src/game/bunching_setup.cpp:481` — apply the one-line change
  above in both evaluators. The bunching path divides by `bunching_num_combinations_`; the premium
  sits inside the same numerator so that pairing is preserved (`STATUS.md:122-127`).

### 3. Library: exploitability must stop assuming zero-sum

`compute_exploitability` (`include/pfs/utility.hpp:285-294`) already has a non-zero-sum path for
rake — `(mes_ev[0]-current_ev[0]) + (mes_ev[1]-current_ev[1])`, which is textbook NashConv. Route
premium games down it:

- `include/pfs/interface.hpp:67` — add `bool is_zero_sum() { return true; }` to `GameBase`.
- `include/pfs/interface.hpp:103` — add `{ g.is_zero_sum() } -> std::convertible_to<bool>;` to the
  `GameLike` concept.
- `include/pfs/utility.hpp:290` — gate on `game.is_zero_sum()` instead of `!game.is_raked()`.

`KuhnGame` and `LeducGame` derive from `GameBase` (`tests/test_kuhn.cpp:50`,
`tests/test_leduc.cpp:81`) so they pick up the default. Keep `is_raked()` as it is — it is a
faithful port and its one caller moves to the new predicate. Note the exploitability check now
costs four tree walks instead of two for premium games, as it already does for raked ones.

### 4. Library: the fold row in the interpreter

`src/game/interpreter.cpp:599-607` forces fold rows to `0.0f`. Today that is not just a display
convention but an exact identity: the fold cfvalue normalises to `-half_pot`, and the add-back at
`:611-612` cancels it to `bias`, which is 0 for a player who can fold. With a premium the true
value is `-extra*node.amount()` — the premium on chips already committed, which is correct and
self-consistent (the kink has to sit at one fixed stack for the tree to be a game at all).
Keeping the hard-coded zero would hide it and make `expected_values()` disagree with
`compute_current_ev` and with what the solver optimised.

Make the shortcut conditional, so premium-0 behaviour is untouched:
```cpp
const bool zero_folds = risk_loss_extra(player) == 0.0;
const bool is_fold = zero_folds && have_actions && node.play(action).prev_action().kind == ActionKind::Fold;
```

### 5. Library: serialization

`deserialize` calls `finalize()` to regenerate counterfactual values from the saved strategy
(`src/game/serialization.cpp:454-458`), which re-runs terminal evaluation. If the premium is not
persisted, a reloaded premium solve silently yields risk-neutral values against a risk-averse
strategy — wrong in a way that looks plausible.

- `src/game/serialization.cpp:161-185` / `:187-217` — write and read the two doubles in
  `write_tree_config` / `read_tree_config`, in the same position in both.
- `include/pfs/file.hpp:30` — bump `kFileFormatVersion` 1 -> 2. The check at `src/file.cpp:96` is
  exact equality, so old files are rejected cleanly instead of mis-parsed from `rake_cap` onward.

### 6. Solver application

- `solver/src/game_file.hpp` / `game_file.cpp`
  - Read an optional top-level `riskPremiumPercent: { "OOP": 2.0, "IP": 0.0 }` in `read_game_file`
    (`:112-143`) and in `read_range_file` (`:132-172`), next to `ranges`. Use `contains`/`value`,
    **not** `.at()`, so existing files keep working. Accept format version 3 as well as 1 and 2 for
    both formats.
  - Store as a percent on `GameFile` and convert to the `TreeConfig` fraction once, so the library
    keeps its fractions-only convention.
- `solver/src/main.cpp`
  - `--risk-premium <oop> <ip>` — one option, two arguments, since the players' premiums are
    independent. Follow the existing argument-loop shape (`:112-161`) but require `i + 2 < argc`:
    ```
    } else if (arg == "--risk-premium" && i + 2 < argc) {
        for (size_t p = 0; p < 2; ++p)
            if (!parse_premium(argv[++i], premium[p]))
                return fail(std::string("invalid risk premium '") + argv[i] + "' (0 to 50 percent)");
    ```
    `parse_premium` wraps the existing `parse_percent` (`:88-92`) and adds the `< 50` bound, with a
    message naming the limit. Report both values in the error context so it is clear which of the
    two arguments was wrong.
  - Apply after the files are read (`:180-191`) and before `build_action_tree` (`:192`), writing
    into `file.value().tree_config.risk_premium`.
  - Extend the usage text (`:30-70`), and print the premiums and their bubble factors in the
    non-quiet summary line so a premium solve is never mistaken for a chipEV one.
- `solver/src/result_file.cpp:110-133` `add_solve_fields` — echo
  `"riskPremiumPercent": {"OOP":…, "IP":…}` plus the derived bubble factors. This one function
  feeds both the result JSON and the solution metadata (`solution_file.cpp:614`). Bump the result
  format version 2 -> 3 (`result_file.cpp:140`) and the solution metadata minor version 1.0 -> 1.1
  (`solver/src/solution_format.hpp:18`), which is a backward-compatible metadata addition per
  `documentation/SolverFileFormat.md:638`.

### 7. Tests

- `tests/test_action_tree.cpp:91-108` — extend the validation table: negative, `0.5`, `> 0.5`, NaN,
  and the premium-with-large-rake rejection.
- `tests/test_game.cpp` — a closed-form payoff test modelled on `always_lose_raked` (`:276-287`):
  a forced-loss range on a tree with a bet, so the EV shift is hand-computable as `-extra*amount`.
- A behavioural test for the semantics: solve a spot where a player faces an all-in and check the
  call threshold moves from `0.5` toward `BF/(1+BF)`; plus a monotonicity test that raising the
  premium does not increase the stack-off frequency. Monotonicity is the robust assertion here —
  prefer it to a tight magic number.
- An asymmetric case: a premium for one player only must leave the other player's own loss
  payoffs untouched, and must still change that player's strategy through the opponent's changed
  behaviour.
- `is_zero_sum()` false with a premium, and `compute_exploitability` taking the NashConv branch.
- `tests/test_file_io.cpp` — round-trip test asserting `tree_config().risk_premium` survives **and**
  that post-load root EVs match pre-save (this is the finding in step 5; no existing test asserts
  any `TreeConfig` field survives, so a write/read mismatch is currently invisible).
- `expected_values()` equals the strategy-weighted `expected_values_detail()` with a premium, and
  fold rows are non-zero.
- Compressed vs uncompressed agreement at a moderate premium (see Limitations).
- `solver/tests/test_game_file.cpp` — the JSON key in both the game and range file templates,
  including absent/present and the CLI-override precedence, and that the two values stay in
  `[OOP, IP]` order through every layer.

### 8. Documentation

- `README.md` — `TreeConfig` snippet (`:78-84`), CLI synopsis and option table (`:115-164`), and a
  short "Risk premium" subsection with the BF mapping, the general-sum caveat, and the limitations.
- `STATUS.md` — a new numbered deviation in `:36-88`: the Rust reference has no risk premium; the
  `GameLike` concept gained `is_zero_sum()`; `kFileFormatVersion` is now 2. Update the test count
  at `:21`.
- `CLAUDE.md` — mention the premium in the file-exchange bullet.
- `documentation/SolverFileFormat.md` — the new metadata key, the minor version, and an amendment
  to the "fold actions are 0" convention at `:424-425`.
- `solver/src/game_file.hpp` and `result_file.hpp` header format comments.

## Limitations to document

- **CFR has no Nash guarantee here.** With asymmetric risk aversion the game is genuinely
  general-sum: a line that raises pot variance at constant mean lowers *both* players' utilities,
  so the players share an interest in small pots. Discounted CFR self-play converges to a Nash
  equilibrium only in two-player zero-sum games. Expect NashConv to possibly plateau above target
  and not to be monotone, and the solution not to be unique. `solve()` is iteration-bounded so it
  cannot hang. Document the returned number as NashConv/2, not a certified exploitability.
- **Only postflop chips are priced.** `node.amount()` covers this tree only; the preflop
  investment sits in `starting_pot` and is treated as sunk. That is consistent with the reference
  point (stack at the start of the postflop hand), and it means BF should be calibrated at that
  stack. A big-pot/short-stack spot will show little risk aversion because little is left to lose.
- **Extreme rake.** With `rake > starting_pot` a tie, and with `rake_rate > 0.5` even a win, can
  have negative `net`, which this implementation does not premium. Both are far outside real rake
  structures; step 1 rejects the combination rather than leaving the assumption implicit.
- **16-bit compression loses precision.** `encode_signed_slice` scales by the slice absolute
  maximum (`src/core/encoding.hpp:64-72`), so there is no overflow risk, but premium-inflated
  losing values raise the scale and cost every other value in the slice up to `log2(BF)` bits.
  Discourage `--compress` at large premiums.

## Verification

1. **Bit-identity first.** Before touching payoffs, record the baseline:
   `ctest --test-dir build-vscode -C Release -LE slow --output-on-failure` (both suites), and keep a
   copy of a result JSON and a `.pfsol` for `simulations/input/KcQdJs_basic_game.json`. After the
   change, with no premium set, both suites must pass unchanged and the outputs must compare equal
   byte for byte apart from timings. Also run `PFS_RUN_SLOW=1` for the PioSOLVER presets. Per
   `CLAUDE.md`, do not edit pinned expectations to make anything pass.
2. **Semantics.** Run the new threshold test and confirm the all-in call threshold matches
   `BF/(1+BF)` within solver tolerance at RP = 0, 2, 5, 10%.
3. **Convergence experiment** (one-off, record in `STATUS.md`): solve
   `simulations/input/stack-42bb-pot-6.5bb.json` with `--ranges mp_raise_lp_call.json --board KcQdJs`
   at RP = 0, 2, 5, 10% and tabulate NashConv against iterations, to confirm it still converges and
   to see how much slower. This is the main risk in the whole change.
4. **Compressed vs uncompressed** agreement on the same spot at RP = 5%.
5. **Round trip.** Save and reload a premium solve; root EVs must match pre-save, and an old
   format-version file must be rejected cleanly.
6. **End to end.** `postflop-solver` on the example game with `--risk-premium 2 5`, checking that
   the premiums and bubble factors appear in the result JSON and the solution metadata in
   `[OOP, IP]` order, and that a higher premium reduces the stack-off frequency in the output.
7. Build warning-free (`/W4` on MSVC) in the default configuration and with
   `PFS_ENABLE_THREADS=OFF`, `PFS_CUSTOM_ALLOC=ON`.

## Follow-up outside this repository

`poker-tools` writes both affected formats — `gto-pluss-exporter/src/GameExport.h` (game file) and
`hrc-analyzer/src/RangeExport.h` (range file, and HRC is itself an ICM tool that knows the
premiums). `CLAUDE.md` requires the other repository's reader or writer to be updated when a shared
format changes. Reading the optional key is backward compatible, so nothing breaks immediately, but
emitting it is a separate task in that repository and needs your go-ahead before I touch it.
