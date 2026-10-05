# Solution file format

Format id `postflop-solver/solution`, version 1.0, extension `.pfsol`. `postflop-solver --solution`
writes it (`solver/src/solution_file.hpp`), and `solver/src/solution_reader.hpp` is a C++ reader.
The writer does not use zstd or the byte-shuffle filter, and it does not write NormalizedWeight
fields. All three are part of the format, and readers should support them.

The result JSON (`solver/src/result_file.hpp`) only holds the first street. This format holds the
complete solved tree, every street and every dealt card, so that other applications can browse
it. It is designed so that:

- a reader never has to load the whole file. Each node can be loaded with a few small reads, using
  file seeks or HTTP range requests (`Range: bytes=a-b`).
- it can be parsed in C++ and in JavaScript (browser or Node.js) with standard tools only: a
  `DataView`, typed arrays, `JSON.parse` and the browser's built-in `DecompressionStream`.
- the writer can stream the file to disk while it walks the solved game. It never needs more than
  a few segments in memory.
- the writer can **prune** the tree, leaving out the subtrees behind actions that are rarely
  taken (§7). Readers can tell a pruned node from a missing one.

## 1. Main idea: the action tree times the dealt cards

The solver's game tree is one abstract **action tree** repeated for every dealt card. The betting
options after `Bet(5300) Call` on the turn are the same whichever turn card was dealt, and only
the strategies differ. The file uses this:

- The action tree is stored once, as a **template**: a small JSON tree in the metadata, usually a
  few hundred nodes.
- The template is split into **groups**. A group is the part of the tree that is played on one
  street without a card being dealt. The root group is the first street. Every chance node of the
  template starts a new group on the next street.
- A **segment** is one group on one board. There is one flop segment. There is one turn segment
  per (flop chance node, turn card), and one river segment per (turn chance node, turn card, river
  card).
- Each segment has a small fixed-size binary **segment table**:
  - a reference to the data blob of every node in the group;
  - for every chance node that leaves the group, a 52-entry **chance table** indexed by card,
    which points to the child segments.

```
 header ──► metadata (JSON: template tree, hands, config)
   │
   └──► root segment (flop)
          slots: [blob OOP:0] [blob IP:0] [blob IP:1] ...
          exits: chance node "Bet Call"  ─► [2c][2d]...[Ah] ─► turn segment (Bet Call, 7h)
                 chance node "Check Check" ─► ...               slots: [blob] [blob] ...
                                                                exits: ─► river segments
```

A node is addressed by **(template node id, turn card, river card)**. Reading it takes one read
per street to get the segment tables, then one read for the node blob. That is at most 5 small
reads for a river node, including the header and metadata, and earlier levels can be cached.

## 2. Conventions

- **Byte order**: little-endian everywhere.
- **Offsets** are absolute byte offsets from the start of the file, stored as `u64`. In JavaScript
  use `Number(view.getBigUint64(o, true))`. Offsets stay below 2^53, so this conversion is exact.
- **Alignment**: the writer starts every segment table and blob at an offset that is a multiple of
  8, with zero padding in between. Inside a decoded node payload, every field starts at a multiple
  of 8 relative to the payload start. A decompressed payload is a fresh buffer, so
  `new Float32Array(buf, offset, n)` and `new Uint16Array(...)` can be used directly. With codec
  `none` the payload starts 8 bytes after an 8-aligned blob, so it is also 8-aligned in the file
  (mmap in C++).
- **Cards**: `card = 4 * rank + suit`. Rank: `2` = 0 … `A` = 12. Suit: `c` = 0, `d` = 1, `h` = 2,
  `s` = 3 (`include/pfs/card.hpp`). `0xFF` means not dealt.
- **Hands**: each player has a fixed hand list, the same for every node of the file. It is the
  solver's `private_cards(player)`: the combos with a nonzero range weight that do not conflict
  with the initial board. Every per-hand array has exactly `H[player]` entries in this order.
  Hands are written high card first in JSON (`"AhKh"`), like the result file. Hands blocked by
  turn or river cards keep their place and have weight 0 there.
- **Actions** are strings in the solver's `Action::to_string()` form (`"Check"`, `"Bet(5300)"`,
  `"AllIn(18000)"`), in the solver's order for the node. This is not necessarily the game file's
  order.
- **Amounts and EVs** are integers or floats in game units: GTO+ chips times `chipScale`, as in
  the game and result files.
- **Strings** are UTF-8.
- **CRC-32** is the IEEE/zlib polynomial (`0xEDB88320`, reflected). It is the same as zlib's
  `crc32()` and fits in a 10-line table-driven function in JavaScript.

### Common structures

**BlobRef** (16 bytes): a reference to a blob (§4).

| Offset | Type | Field    | Meaning                                                   |
|-------:|------|----------|-----------------------------------------------------------|
| 0      | u64  | offset   | Absolute offset of the blob envelope                      |
| 8      | u32  | length   | Stored length in bytes, envelope included. 0 = no data    |
| 12     | u32  | crc32    | CRC-32 of the stored bytes, or 0 if the file has no CRCs   |

**SegmentRef** (24 bytes): one entry of a chance table, or the root segment.

| Offset | Type | Field       | Meaning                                                              |
|-------:|------|-------------|----------------------------------------------------------------------|
| 0      | u64  | offset      | Absolute offset of the segment table                                 |
| 8      | u32  | tableLength | Length of the segment table in bytes                                 |
| 12     | u32  | spanLength  | Length of the table plus this segment's node blobs, which follow it contiguously (§11) |
| 16     | u32  | crc32       | CRC-32 of the segment table, or 0                                    |
| 20     | u8   | kind        | See below                                                            |
| 21     | u8   | suitPerm    | Suit permutation (§6). `0xE4` = identity                             |
| 22     | u16  | reserved    | 0                                                                    |

`kind`:

| Value | Name       | Meaning                                                                       |
|------:|------------|-------------------------------------------------------------------------------|
| 0     | Impossible | The card cannot be dealt: it is on the board, or no combination of the two ranges survives it. All other fields are 0 |
| 1     | Segment    | A segment is stored at `offset`                                               |
| 2     | Empty      | The card is possible but the next group has no nodes with data. No segment is stored. The writer never produces this, because a street reached after an all-in call is a terminal node in the template |
| 3     | NotStored  | The card is possible but the writer did not store this street (`storedStreets`) |
| 4     | Pruned     | The card is possible but the chance node is in a pruned subtree (§7). No segment is stored |

Readers treat unknown kinds as `NotStored`.

## 3. File layout

```
[Header, 72 bytes]
[Metadata blob]                  written right after the header
[Root segment table][its node blobs]
  [Child segment table][its node blobs]
    [Grandchild segment table][its node blobs] ...   depth-first, pre-order
```

Only the header has a fixed position. Everything else is reached through references, so readers
must not assume this order. The order is the writer's recommendation for locality (§11).

### Header (72 bytes, offset 0)

| Offset | Type       | Field        | Meaning                                                     |
|-------:|------------|--------------|-------------------------------------------------------------|
| 0      | u8[8]      | magic        | `89 50 46 53 4F 4C 0D 0A` (`"\x89PFSOL\r\n"`)               |
| 8      | u16        | versionMajor | 1. A reader rejects a major version it does not know       |
| 10     | u16        | versionMinor | 0. Minor versions only add things that old readers can ignore |
| 12     | u32        | flags        | bit 0: CRCs are present. Other bits are 0                   |
| 16     | u64        | fileLength   | Total file length. Catches truncated files and downloads    |
| 24     | BlobRef    | metadata     | The metadata blob                                           |
| 40     | SegmentRef | root         | The root segment (`kind` = 1, `suitPerm` = `0xE4`)          |
| 64     | u32        | reserved     | 0                                                           |
| 68     | u32        | headerCrc32  | CRC-32 of bytes 0–67, always present                        |

The magic follows the PNG convention. The high byte and the CR LF detect files that were mangled
by text-mode transfers.

## 4. Blobs

The metadata and the node data are stored as blobs: an 8-byte envelope followed by the stored
payload.

| Offset | Type | Field     | Meaning                                                          |
|-------:|------|-----------|------------------------------------------------------------------|
| 0      | u8   | type      | 1 = metadata (JSON), 2 = node data                               |
| 1      | u8   | codec     | 0 = none, 1 = zlib (RFC 1950), 2 = zstd                          |
| 2      | u16  | reserved  | 0                                                                |
| 4      | u32  | rawLength | Length of the payload after decoding                             |
| 8      | …    | payload   | `BlobRef.length - 8` bytes, encoded with `codec`                 |

Codecs:

- **zlib** (deflate with the zlib header and Adler-32) is the recommended default. Browsers decode
  it natively with `new DecompressionStream("deflate")`, and Node.js with `zlib.inflateSync`. In
  C++ it needs zlib or the single-file [miniz](https://github.com/richgel999/miniz) (MIT).
- **zstd** gives better ratios and is faster to decode. In JavaScript it needs a library, either
  the pure-JS `fzstd` or a WASM build. The library already has `PFS_ENABLE_ZSTD`.
- **none** always works and needs no dependency.

Each blob is compressed on its own, which is what makes random access possible. Codecs can be
mixed in one file. A writer stores a blob raw when compression does not make it smaller.

Segment tables are deliberately **not** blobs. They are stored raw and have a fixed size for their
group, so the writer can reserve their space and fill them in later (§11).

## 5. Metadata (blob type 1, JSON)

The metadata is small: the template, the hand lists and the configuration. Readers load it
entirely. It reuses the field names of the game and result files where they mean the same thing.

```json
{
  "format": "postflop-solver/solution",
  "version": 1,
  "input": "KcQdJs_basic_game.json",
  "source": "KcQdJs_basic.gto",
  "chipScale": 1000.0,
  "board": { "flop": "KcQdJs", "turn": null, "river": null },
  "ranges": { "OOP": "AA-22,AKs-ATs,...", "IP": "JJ-55,AQs-AJs,..." },
  "treeConfig": { "initialState": "flop", "startingPot": 7500, "effectiveStack": 18000,
                  "rakeRate": 0.0, "rakeCap": 0.0 },
  "settings": { "maxIterations": 1000, "targetExploitabilityPercent": 0.25,
                "compression": false, "threads": 8 },
  "solve": { "iterations": 80, "exploitability": 16.989, "exploitabilityPercent": 0.2265,
             "seconds": 3.451, "memoryMB": 113.5 },
  "storedStreets": ["flop", "turn", "river"],
  "pruning": { "minActionFrequencyPercent": 1.0, "minLineFrequencyPercent": 0.0 },
  "hands": { "OOP": ["2d2c", "2h2c", "..."], "IP": ["5d5c", "..."] },
  "initialWeights": { "OOP": [1.0, 1.0, "..."], "IP": [0.2, "..."] },
  "groups": [
    { "id": 0, "street": "flop", "root": 0, "slots": [0, 1, 2, 3, 5, 6], "exits": [3, 6] },
    { "id": 1, "street": "turn", "root": 7, "slots": [7, 8, 9, 10], "exits": [9] }
  ],
  "tree": [
    { "id": 0, "kind": "action", "street": "flop", "group": 0, "slot": 0,
      "parent": null, "parentAction": null, "gameFileId": "OOP:0",
      "player": "OOP", "pot": 7500, "contributed": [0, 0],
      "actions": ["Check", "Bet(5300)"], "children": [1, 2] },
    { "id": 3, "kind": "chance", "street": "flop", "group": 0, "slot": 3, "exit": 0,
      "parent": 1, "parentAction": 0, "pot": 7500, "contributed": [0, 0],
      "children": [7] },
    { "id": 4, "kind": "terminal", "terminal": "fold", "folded": "IP", "street": "flop",
      "group": 0, "parent": 2, "parentAction": 0, "pot": 12800, "contributed": [5300, 0] }
  ]
}
```

- `storedStreets`: the streets whose segments are in the file. If the river is missing, every
  river chance entry is `NotStored`.
- `pruning`: the thresholds the writer pruned with (§7), or `null` if the tree is not pruned. A
  threshold of 0 means that criterion was not used. This is for display only, because readers take
  pruning from the slot states and chance entries.
- `hands` and `initialWeights` define `H[player]` and the hand order of every per-hand array.
- `groups[g]`:
  - `root`: the template id of the group's first node;
  - `slots`: the template ids that have a slot in the segment table, in slot order. These are the
    action and chance nodes of the group, in pre-order;
  - `exits`: the chance nodes of the group, in exit order.
- `tree[i]` is the template node with id `i`. Ids are pre-order indices.
  - `kind` is `action`, `chance` or `terminal`.
  - `parent` / `parentAction`: the parent template node and the index of the action in the
    parent's `actions` that leads here.
  - `children` is parallel to `actions`. A chance node has exactly one child, the root of the
    next group.
  - `pot`: the pot at the node. `contributed`: what OOP and IP have put in beyond the starting
    pot.
  - `slot` and `exit` index into the group's segment table.
  - `gameFileId` (optional) is the matching node id of the input game file, for linking with
    `poker-tools` data.
  - `terminal` is `fold` or `showdown`. Terminal nodes have no slot and no data. As in the
    solver's interpreter, a turn or river reached by calling an all-in is a showdown terminal,
    so the template has no runout chance nodes.

Readers ignore keys they do not know.

## 6. Suit isomorphism and suit permutations

Like the Rust original, the solver does not build separate subtrees for **isomorphic** cards. On
`KcQd7c`, for example, the turn cards `2h` and `2s` are equivalent: the board and both ranges are
symmetric in hearts and spades. Only one of them is solved, and the other is the same data with
two suits swapped. The file keeps this saving. A chance entry for a non-represented card points
to the represented card's segment and carries a **suit permutation** that must be applied when
reading. Data is never duplicated.

All stored data is in the **tree frame**, the suits as the solver's tree names them. The reader
keeps a permutation `π` from the actual suits to the tree frame. It starts as the identity at the
root.

**Encoding.** A permutation is a byte `p` with 2 bits per suit:
`perm(s) = (p >> (2 * s)) & 3`. The identity is `0b11100100 = 0xE4`. Swapping hearts and spades
is `0b10110100 = 0xB4`. Applied to a card: `perm(c) = (c & ~3) | perm(c & 3)`.

**Following a chance entry.** At an exit of the current segment, for an actual card `c`:

1. Translate the card into the tree frame: `t = π(c)`.
2. Take `entry = chanceTable[exit][t]`. The table is indexed by tree-frame cards.
3. Compose: `π' = σ ∘ π`, that is `π'(s) = σ(π(s))`, where `σ = entry.suitPerm`.
4. Continue in the target segment with `π'`.

**Reading per-hand data.** The value for the actual hand `(a, b)` of a player is the stored value
of the hand `(π(a), π(b))`, looked up in the same player's hand list. That hand always exists,
because isomorphism requires symmetric ranges. Readers build a 1326-entry table from pair index to
hand index once per player, then one hand permutation per distinct `π` (at most 24). The pair
index is `card_pair_to_index` from `card.hpp`: with `c1 < c2`, `c1 * (101 - c1) / 2 + c2 - 1`.
Action lists, pots and amounts do not depend on suits and need no translation.

**Writer side.** The writer always walks the tree through represented cards only, so the
interpreter never applies a swap (`turn_swap()` and `river_swap()` stay empty) and every array it
gets is in the tree frame. For a card `t` that is not a child of the chance node,
`isomorphic_chances(node)` gives the represented card `t'` of the same rank. The entry then
points to `t'`'s segment with `σ` = the transposition of `suit(t)` and `suit(t')`. That swap leaves
the tree-frame board unchanged, which is the isomorphism condition, so `π'` maps the actual board
onto the stored segment's board.

## 7. Pruning

Most of a solved tree is lines that are almost never played, for example a raise size used 0.3% of
the time on the flop, with its whole turn and river below it. The writer can leave these subtrees
out. The format does not prescribe the criterion: it only records which nodes were pruned. The
criteria below are what the writer implements (§11), and the thresholds used are recorded in the
metadata (`pruning`).

### Criteria

- **Action frequency** (`--solution-prune <percent>`). At an action node, the frequency `f(a)` of
  action `a` is its frequency over the whole range: the average of the hands' strategies for `a`,
  weighted by the normalized weights. This is the `freq` of the node's Summary field and the
  `rangeFrequencies` of the result file. If `f(a)` is below the threshold, the child reached by `a`
  is pruned, together with everything below it.
- **Line frequency** (`--solution-prune-line <percent>`, optional). `L(n)` is the product of
  `f(a)` over the actions on the path from the root to `n`, where chance nodes count as 1. A node
  with `L(n)` below the threshold is pruned, together with everything below it.
  - Each action multiplies the weight of the remaining compatible hand pairs by exactly its range
    frequency, so `L` is the probability that the line is played, street by street, given the cards
    known on each street.
  - It is not exactly the probability conditioned on the final board, because card removal shifts
    the earlier streets' frequencies slightly. That does not matter for a threshold.
- A node is pruned if either criterion applies. A threshold of 0 disables a criterion. Pruning is
  off by default.

Rules that apply with either criterion:

- **The node where the rare action is taken is kept in full.** That includes the Strategy and
  ActionEv rows of the rare action, so a reader can still show how often the action is taken, with
  which hands, and its EV. Only what follows the action is left out.
- **Every kind of action can be pruned**, including calls: a rare flop call removes all of its turn
  and river segments. An action that leads directly to a terminal node has nothing to prune.
- **The decision is made per segment.** A bet can be pruned on one turn card and kept on another.
  Isomorphic cards share the decision, because they share the data.
- **The frequencies are computed in double**, in a fixed order, from the interpreter's values, the
  same calculation as the Summary field. The decision therefore does not depend on the thread
  count.
- **Readers do not recompute the decision.** The slot states and chance entries in the file are
  authoritative.

A range frequency hides strong hands that take a rare action. If a few nut combos always raise,
the raise can be below 1% for the range while it is 100% for those hands, and their continuation
is pruned. A writer could offer other criteria later, such as a minimum number of combos or the
highest frequency of any single hand, without changing the format.

### How pruning is stored

- In a segment, every slot in a pruned subtree has `slotState` = Pruned (§8), and its BlobRef is all
  zeros.
- Every chance-table entry of a pruned exit has `kind` = Pruned (§2), except cards on the board,
  which stay Impossible. No segments are written below a pruned exit, which is where most of the
  savings come from.
- Terminal nodes have no slot. A terminal node is in a pruned subtree if its parent is pruned.

The savings depend entirely on the solve and have not been measured. As an illustration, in the
example tree of §12, pruning one flop action whose subtree holds one of the 4 flop exits removes
49 turn segments and 11,760 river segments, about a quarter of the file. Pruning one turn action
above an exit removes 48 river segments for that turn card.

## 8. Segment table

| Offset | Type                          | Field     | Meaning                                       |
|-------:|-------------------------------|-----------|-----------------------------------------------|
| 0      | u32                           | magic     | `0x54474553` (`"SEGT"`)                       |
| 4      | u16                           | group     | Group id (§5)                                 |
| 6      | u8                            | street    | 0 = flop, 1 = turn, 2 = river                 |
| 7      | u8                            | reserved  | 0                                             |
| 8      | u8[5]                         | board     | Tree-frame board, `0xFF` for undealt cards    |
| 13     | u8[3]                         | reserved  | 0                                             |
| 16     | u32                           | numSlots  | Must equal `groups[group].slots.length`       |
| 20     | u32                           | numExits  | Must equal `groups[group].exits.length`       |
| 24     | BlobRef[numSlots]             | slots     | Node data per slot. All zeros unless `slotState` is Stored |
| …      | u8[pad8(numSlots)]            | slotState | State per slot, see below. Zero padding up to a multiple of 8 |
| …      | SegmentRef[numExits × 52]     | exits     | Chance table per exit, indexed by tree-frame card |

`slotState`:

| Value | Name      | Meaning                                                                  |
|------:|-----------|--------------------------------------------------------------------------|
| 0     | Stored    | The node's blob is at the slot's BlobRef                                 |
| 1     | NotStored | The writer did not store data for this node, e.g. chance-node data left out |
| 2     | Pruned    | The node is in a pruned subtree (§7)                                     |

Readers treat unknown states as NotStored.

With `pad8(n) = (n + 7) & ~7`, the table size is
`24 + 16 * numSlots + pad8(numSlots) + 1248 * numExits`. The size depends only on the group, so the
writer can reserve it before the contents are known. Pruning leaves the size unchanged.

## 9. Node data (blob type 2)

Every action node and chance node of a segment has one blob, unless its `slotState` is not Stored.
Terminal nodes have none.

### Payload layout

| Offset | Type                   | Field        | Meaning                                   |
|-------:|------------------------|--------------|-------------------------------------------|
| 0      | u32                    | magic        | `0x45444F4E` (`"NODE"`)                   |
| 4      | u32                    | templateNode | Template node id, a consistency check     |
| 8      | u16                    | numFields    |                                           |
| 10     | u16                    | reserved     | 0                                         |
| 12     | u32                    | reserved     | 0                                         |
| 16     | FieldDesc[numFields]   | fields       | 16 bytes each                             |
| …      | …                      | data         | Field data at the offsets of the descriptors |

**FieldDesc** (16 bytes):

| Offset | Type | Field    | Meaning                                                         |
|-------:|------|----------|-----------------------------------------------------------------|
| 0      | u8   | id       | Field id, see below                                             |
| 1      | u8   | player   | 0 = OOP, 1 = IP, `0xFF` = not per player                        |
| 2      | u8   | encoding | See below                                                       |
| 3      | u8   | filter   | 0 = none, 1 = byte shuffle                                      |
| 4      | u32  | count    | Number of values                                                |
| 8      | f32  | scale    | For scaled encodings                                            |
| 12     | u32  | offset   | Offset of the data from the payload start, a multiple of 8     |

### Fields

| Id | Name             | Player  | Count     | Values                                                           |
|---:|------------------|---------|-----------|------------------------------------------------------------------|
| 1  | Strategy         | acting  | `A × H`   | Action-major: `[a * H + h]`, as `PostFlopGame::strategy()`       |
| 2  | ActionEv         | acting  | `A × H`   | EV of each action per hand, as `expected_values_detail()`        |
| 3  | Weight           | each    | `H`       | Reach weight: range weight times the actions taken, 0 if blocked by the board (`weights()`) |
| 4  | NormalizedWeight | each    | `H`       | `normalized_weights()`: the weight times the opponent combos the hand does not block |
| 5  | Ev               | each    | `H`       | `expected_values()`                                              |
| 6  | Equity           | each    | `H`       | `equity()`                                                       |
| 7  | Summary          | `0xFF`  | `6 + A`   | f32: `[evOOP, evIP, equityOOP, equityIP, combosOOP, combosIP, freq_0 … freq_{A-1}]` |

- `A` is the number of actions of the node, `H` = `H[player]`.
- In Summary, EV and equity are averages weighted by the normalized weights, `combos` is the sum
  of the reach weights, and `freq` are the range frequencies of the actions, as `rangeFrequencies`
  in the result file. At chance nodes `A` = 0.
- Fields 1 and 2 only exist at action nodes. At chance nodes the default set is 3, 5, 6 and 7.
- **Default set**: 1, 2, 3, 5, 6 and 7 (one copy of 3, 5 and 6 per player). Field 4 is optional.
  Weights can be derived from the ancestors' strategies, and normalized weights from the weights,
  but storing them makes every node readable on its own with a single read.
- A reader ignores field ids it does not know. New fields are minor-version changes.
- Values for hands with weight 0 are written as 0 and have no meaning. Non-finite values are
  written as 0.
- EV conventions (fold actions are 0, pot bias included) are the interpreter's, the same as
  `evs` in the result file.

### Encodings

| Value | Name       | Size | Decoded value                  | Default for                 |
|------:|------------|-----:|--------------------------------|-----------------------------|
| 0     | F32        | 4    | the float                      | Summary; everything with `--solution-precision f32` |
| 1     | U16Unit    | 2    | `q / 65535`                    | Strategy, Equity            |
| 2     | I16Scaled  | 2    | `q * scale` (q signed)         | ActionEv, Ev                |
| 3     | U16Scaled  | 2    | `q * scale`                    | Weight, NormalizedWeight    |

The quantized encodings follow the solver's own compression mode (`--compress`), which stores
strategies as `u16` and values as `i16` with a scale.

- I16Scaled and U16Scaled: `scale = maxAbs / 32767` or `max / 65535` over the field, and
  `q = round(v / scale)`. If `maxAbs` = 0, then `scale` = 0 and all `q` are 0.
- Strategy with U16Unit: the writer rounds each hand's column with the largest-remainder method,
  so the action frequencies of a hand add up to exactly 65535. Readers can then rely on sums of 1.
- Precision: strategies to 1.5e-5, equity the same, EVs to 1/32767 of the largest absolute EV at
  the node (about 0.3 chips per 10,000 chips), and weights to 1/65535 of the largest weight at the
  node.
- F32 reproduces the interpreter's values exactly. Use it when the file is used to compare
  solvers.

**Byte shuffle** (filter 1) stores the bytes of the `count` values of a field as planes: every
value's byte 0, then every value's byte 1, and so on. It is undone after decompression. The
writer does not use it. On the example game in §12 it made the zlib output larger: 62.4 MB
instead of 60.1 MB for u16, and 102.8 MB instead of 96.0 MB for f32.

## 10. Reading

### Opening

1. Read the header. Over HTTP, request the first 64 KiB in one go: the writer places the metadata
   right after the header, so it usually comes along. Check the magic, the major version, the
   header CRC and `fileLength`.
2. Read and decode the metadata blob. Build the template, the hand lists and the pair-index
   tables.
3. Read the root segment. Use `spanLength` to get its node blobs in the same request.

### Loading a node `(templateId, actual turn, actual river)`

1. Follow the template upward to find the exits on the way. A group root's `parent` is the chance
   node, an exit of the previous group.
2. Start with `segment = root` and `π = identity`.
3. For each exit on the path from the root: take the dealt card for that street, apply §6 to get
   `entry` and the new `π`, and stop if `entry.kind` is not `Segment`. Report the node as pruned,
   not stored, empty or impossible, depending on the kind. Otherwise load the segment at
   `entry.offset`, and cache it.
4. Let `s = tree[templateId].slot`. If `segment.slotState[s]` is not Stored, report the node as
   pruned or not stored. Otherwise read `segment.slots[s]`, decode the blob, undo the filters and
   encodings, and remap the per-hand arrays with `π`.

A browser UI can show pruned lines greyed out. The template still has their actions and amounts,
and the parent node still has the frequency and EV of the action that leads to them.

### HTTP range requests

- The server must support `Range` and answer `206 Partial Content`. Static servers, S3,
  CloudFront and most CDNs do. If a reply is `200`, the server ignored the range, and the reader
  should give up rather than download gigabytes.
- Serve the file as `application/octet-stream` **without** on-the-fly `Content-Encoding`. The
  blobs are already compressed, and dynamic compression combined with ranges is unreliable.
- A single-range `Range` header is a CORS-safelisted request header, so cross-origin readers need
  no preflight.
- Batch work where possible: one request per segment (`spanLength`), and a small LRU cache of
  segments and decoded nodes. With HTTP/2 many small requests are cheap.

### JavaScript sketch

```js
async function readRange(url, offset, length) {
  const res = await fetch(url, { headers: { Range: `bytes=${offset}-${offset + length - 1}` } });
  if (res.status !== 206) throw new Error(`range requests not supported (${res.status})`);
  return new Uint8Array(await res.arrayBuffer());
}

async function decodeBlob(bytes) {             // bytes = the whole stored blob
  const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const codec = v.getUint8(1), rawLength = v.getUint32(4, true);
  const stored = bytes.subarray(8);
  if (codec === 0) return stored.slice();      // copy => aligned fresh buffer
  if (codec !== 1) throw new Error(`codec ${codec} not supported`);
  const stream = new Blob([stored]).stream().pipeThrough(new DecompressionStream("deflate"));
  const raw = new Uint8Array(await new Response(stream).arrayBuffer());
  if (raw.length !== rawLength) throw new Error("bad blob length");
  return raw;
}

function readField(raw, desc) {                // desc decoded from a FieldDesc
  let bytes = raw.subarray(desc.offset, desc.offset + desc.count * (desc.encoding === 0 ? 4 : 2));
  if (desc.filter === 1) bytes = unshuffle(bytes, desc.encoding === 0 ? 4 : 2);
  const buf = bytes.slice().buffer;            // aligned copy
  switch (desc.encoding) {
    case 0: return new Float32Array(buf);
    case 1: return Float32Array.from(new Uint16Array(buf), q => q / 65535);
    case 2: return Float32Array.from(new Int16Array(buf), q => q * desc.scale);
    case 3: return Float32Array.from(new Uint16Array(buf), q => q * desc.scale);
  }
}
```

For local files in a browser, use `file.slice(start, end).arrayBuffer()` on a `File` from an
`<input type="file">`. It reads only that range. In Node.js use `fs.promises.open()` and
`handle.read(buffer, 0, length, position)`. In C++, use `std::ifstream::seekg` and `read`, or
memory-map the file. For HTTP from C++, set libcurl's `CURLOPT_RANGE`.

## 11. Writing

The writer is part of `postflop-solver` (`solver/src/solution_file.{hpp,cpp}`). It uses miniz
(`third-party/miniz`, MIT) for zlib.

```
postflop-solver game.json [--output result.json] --solution result.pfsol
    [--solution-streets flop|turn|river]      deepest street stored, default river
    [--solution-precision u16|f32]            default u16
    [--solution-codec zlib|none]              default zlib
    [--solution-prune <percent>]              prune below this action frequency, default 0 (off)
    [--solution-prune-line <percent>]         prune below this line frequency, default 0 (off)
```

Example: `--solution-prune 1` leaves out everything after an action that the range takes less
than 1% of the time.

The writer:

1. Builds the template and the groups by walking the solved game from the root, following the
   first represented card at every chance node. The topology is the same for every card. It
   writes a placeholder header and then the metadata.
2. Writes segments depth-first, in pre-order. For each segment:
   1. reserve its table, of known size;
   2. write the node blobs of its slots, so that they follow the table contiguously and
      `spanLength` covers them;
   3. recurse into each exit and card;
   4. seek back and fill in the table.
   Only one table per street is in memory at a time.

   Pruning fits into this walk. The writer visits the nodes of a group in pre-order and carries a
   pruned flag and the line frequency `L` down the tree:
   - a node's action frequencies are computed with its Summary, so they are known before its
     children are visited;
   - when a child is pruned, the writer marks the child's slots in the segment as Pruned and fills
     the chance tables of its exits with Pruned;
   - the writer skips the pruned nodes entirely, so it neither computes nor recurses into them. A
     pruned export is therefore also faster.
3. Seeks back and writes the header with the root reference, `fileLength` and the CRC.

The output must be seekable, so it can be a file but not a pipe. Node values come from the
interpreter: `apply_history` to the node, `cache_normalized_weights()`, then `strategy()`,
`expected_values_detail()`, `weights()`, `expected_values()` and `equity()`. The blobs of a
segment are compressed in parallel and written in slot order, so the file is identical for
every thread count, like the solve itself. Exporting reads the solved game and does not change
it, so it cannot affect the solver's results.

The tests (`solver/tests/test_solution_file.cpp`) cover a monotone flop and a game that starts
on the turn. They read every node back on boards with isomorphic turn and river cards and
compare it with the interpreter, which applies its own suit swaps:
- strategies and weights must match exactly in f32 files;
- EVs and equities must match to within rounding;
- u16 values must match to within one quantization step.

## 12. Size estimates

Per action node, with `A` actions, `H_p` hands for the acting player, `H0 + H1` hands in total and
2-byte values, using the default fields:

```
raw ≈ 2·A·H_p · 2 B   (Strategy + ActionEv)
    + 3·(H0 + H1) · 2 B   (Weight + Ev + Equity, both players)
```

With 800 hands per player and 3 actions, that is about 19 KB raw, or 38 KB with f32.

A tree with 4 flop exits, 5 turn exits per turn segment, about 15 nodes per group and a rainbow
flop (49 turn cards, 48 river cards):

| Street | Segments                 | Nodes             | Raw, u16 |
|--------|--------------------------|-------------------|---------:|
| Flop   | 1                        | ~10               | 0.2 MB   |
| Turn   | 4 × 49 = 196             | ~2,900            | 56 MB    |
| River  | 196 × 5 × 48 = 47,040    | ~706,000          | 13 GB    |

The segment tables add about 15 MB, roughly 0.1%. The river is over 99% of the file, and the
total is about the solver's own memory use for the game, before compression. That is why
`--solution-streets turn` and pruning (§7) are worth having, and why random access is required
rather than optional. Pruning a flop or turn line saves much more than pruning a river line,
because each pruned exit removes every segment below it.

### Measured

`simulations/input/KcQdJs_basic_game.json`, a rainbow flop with 132 OOP and 103 IP combos and
one bet size per street. The solve takes 3.3 seconds and 113.5 MB. The tree has 11,908
segments and 52,684 nodes. Times are with 8 threads.

| Options                    | File    | Nodes stored | Write time |
|----------------------------|--------:|-------------:|-----------:|
| defaults (u16, zlib)       | 60.1 MB | 52,684       | 4.1 s      |
| `--solution-codec none`    | 132.1 MB| 52,684       | 2.5 s      |
| `--solution-precision f32` | 96.0 MB | 52,684       | 4.0 s      |
| `--solution-streets turn`  | 1.4 MB  | 940          | 0.2 s      |
| `--solution-prune 1`       | 58.0 MB | 49,112       | 3.9 s      |
| `--solution-prune 5`       | 56.1 MB | 47,099       | 3.5 s      |
| `--solution-prune-line 1`  | 46.1 MB | 36,625       | 3.1 s      |
| `--solution-prune-line 5`  | 25.1 MB | 18,260       | 1.8 s      |

- zlib compresses the u16 nodes about 2.2 times.
- Pruning by action frequency saves little here, because this tree has only one bet size per
  street. In trees with several sizes the rarely used ones are the ones pruned.
- About 50 µs per node goes to the interpreter (`apply_history` from the root, the EVs and the
  equities), and the rest to compression.

## 13. Compatibility and versioning

- **Major version** changes break readers. **Minor versions** may add metadata keys, field ids,
  encodings or entry kinds, all of which old readers ignore or treat as "not stored".
- The format is independent of `save_game_to_file` (`include/pfs/file.hpp`, magic `PFSC`).
  `save_game_to_file` dumps the solver's internal state so that a solve can be resumed, needs this
  library to interpret, and cannot be read partially. This format holds only the results, in a
  documented layout.
- The result JSON stays as it is. It remains the small, easy format for the first street.
- A JavaScript reader in `../poker-tools` must be written from this specification, not ported from
  this repository's code (see `CLAUDE.md` on licenses). Reading a data file does not link the
  library.

## 14. Decisions and open questions

Decided with the implementation:
- miniz is vendored into `third-party/miniz` and always built, so zlib is the default codec.
- u16 is the default precision. It makes the file 37% smaller than f32 (§12).
- The writer does not byte shuffle (§9).

Open:

1. **zstd.** The format defines it, but the writer does not produce it. zstd would need a
   library in JavaScript.
2. **Default fields.** Dropping Weight (derivable from the ancestors' strategies) saves about 15%
   but makes readers fetch the ancestors.
3. **Write speed.** The writer replays the interpreter from the root for every node. The cost
   per node grows with the number of hands. At the example's 120 hands per player, a tree with
   700,000 river nodes would take about a minute, and wide ranges take several times longer.
   Reusing the interpreter state between siblings would make it faster.
4. **Chance node reports.** A field with per-card summaries (EV and equity of each player for
   every dealt card) at chance nodes would make the "turn report" view possible without fetching
   49 segments. It is not in version 1.0, but would fit as a new field id.
5. **Which actions pruning applies to.** The writer prunes any action, calls included. It could
   instead be limited to bets, raises and all-ins, or have a separate threshold for calls.
6. **Pruning criterion.** A range frequency hides rare actions that some hands always take (§7).
   An alternative or additional criterion would keep an action if any hand takes it with more than
   a given frequency, or if it carries more than a given number of combos.
