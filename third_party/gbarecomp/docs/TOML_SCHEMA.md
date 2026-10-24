# TOML Schema — gba_recompile per-binary config

Status: draft v1 (2026-05-24).

The `gba_recompile` tool consumes one TOML file per binary
(`--config <path>`). The file supplements the function-finder's
automated walk: manual function entries are deduplicated against
discovered ones, and data ranges hard-exclude byte ranges from
being decoded as code.

## Power-user contract

Entries are **not validated for correctness.** A wrong address in
`[[extra_func]]` will generate junk code; a too-narrow `[[data_range]]`
will let the finder walk into literal pools. That's the cost of the
escape hatch — the user owns the consequences.

The tool rejects only entries that **structurally contradict each
other** (same address declared both `extra_func` and `exclude_func`,
overlapping `data_range` + `extra_func`, mode mismatch between
finder and manual). Everything else is the user's call.

## False-positive policy

This is the load-bearing rule that shapes the schema:

> Missing a function is a **miss** — surfaces as a `runtime_dispatch_miss`
> at runtime, points at the exact PC to add. Loud, addressable, fixable.
>
> Decoding data as code is a **false positive** — generates junk C, may
> silently dispatch into the junk, corrupts state, masks real bugs.
> Bypasses the L2 oracle's diff. Catastrophic.

Misses are cheap; false positives are expensive. The finder leans
conservative; the TOML closes the gap.

## File structure

```toml
[program]
name         = "GBA BIOS"
id           = "gba_bios"
load_address = 0x00000000
size         = 0x00004000
entry_pc     = 0x00000000
speculative_literal_harvest = false # optional; default true
aot_scan_start = 0x080000C0         # optional executable-ROM sweep start
aot_scan_end   = 0x080A0000         # exclusive; both bounds required
static_resume_all = true            # every native instruction is IRQ-resumable
# codegen_shards = 16               # optional cartridge override; default auto

[identity]
sha1 = "300c20df6731a33952ded8c436f7f186d25d3492"   # required, abort on mismatch
md5  = "a860e8c0b6d573d191e4ec7db1b1e4f6"           # optional cross-check

[[extra_func]]
addr = 0x00000000
mode = "arm"
name = "reset_vector"           # optional; default: afunc_XXXXXXXX / tfunc_XXXXXXXX
note = "BIOS reset entry"       # optional documentation

[[extra_func]]
addr = 0x00000B6C
mode = "thumb"
resume = true                    # optional interior exception-return alias
note = "IRQ return into a containing function"

[[resume_range]]
start = 0x08001000               # real host entry
end   = 0x08001020               # exclusive, aligned end
mode  = "thumb"
note  = "Observed interruptible hot function"

[[thumb_alu_immediate_override]]
addr = 0x08002010                # exact THUMB ALU-immediate instruction PC
note = "Opt-in game enhancement constant"

[[mod_function_hook]]
addr = 0x08003000                 # exact guest subroutine entry
mode = "thumb"                   # arm | thumb
note = "Trusted mod replacement seam"

[[data_range]]
start = 0x000001A0
end   = 0x000001C0              # [start, end) — exclusive upper bound
note  = "Nintendo logo pattern bytes"

[[code_copy]]
runtime_start = 0x030056F0
source_start  = 0x080B197C
size          = 0x00001280
name          = "iwram_funcs"    # optional documentation
note          = "ROM code copied to IWRAM before execution"

[[jump_table]]
addr         = 0x00000168
stride       = 4
count        = 43
format       = "abs32"           # also abs16, pcrel_thumb, pcrel_arm, u8_rel_thumb, u16_rel_thumb
entries_mode = "arm"
name         = "swi_dispatch_table"
note         = "SWI 0x00..0x2A handlers"

[[exclude_func]]
addr   = 0x080FFE40
reason = "literal pool walked as code by finder pre-vX.Y"
```

## Section reference

### `[program]` (required)

| key            | type    | meaning |
|----------------|---------|---------|
| `name`         | string  | Human-readable label, appears in log output. |
| `id`           | string  | Short slug. Used in output filenames. |
| `load_address` | int hex | Where the binary's first byte maps in CPU memory. |
| `size`         | int hex | Byte size of the binary (must match the file on disk). |
| `entry_pc`     | int hex | Initial PC. Seeded as `[[extra_func]]` automatically; declaring it again is a redundant no-op. |
| `speculative_literal_harvest` | bool | Optional, default `true`. Harvest plausible callback pointers from PC-relative literal loads. Setting `false` disables weak pointer candidates. Referenced Thumb PUSH-LR entries with validated instruction bytes remain discoverable, including untagged copy sources and ADR references, so their absolute ROM branches and switch cases are compiled too. Direct branch discovery is unchanged. |
| `aot_scan_start` / `aot_scan_end` | int hex | Optional half-open executable-ROM range. Reachable PC-relative literals can identify callback tables. Referenced ROM objects outside the range also qualify when their first two words are distinct Thumb callbacks: at least one PUSH-LR entry, paired with another PUSH-LR entry or a short stackless RAM-global leaf (store and BX LR, no calls or other branches). Separately address-taken leaves must use the exact RAM-global literal corroborated by a descriptor. Only the two descriptor words are inspected; no table end is guessed. Keep compressed graphics and other assets outside the executable range. Omit both fields to disable table/descriptor discovery; strongly qualified directly referenced Thumb entries remain enabled. A configured range also bounds those entry candidates. |
| `static_resume_all` | bool | Optional, default `true`. Add a static dispatch/resume entry for every aligned instruction inside each emitted native body. IRQ/SWI returns and host-stack unwinds may resume at any instruction boundary. Explicitly disabling this is for discovery experiments and does not guarantee strict end-to-end AOT coverage. |
| `codegen_shards` | int | Optional cartridge override, range 2..256. When omitted or `0`, the recompiler chooses an adaptive power-of-two count from 2..64. Cartridge output is never monolithic. `1` is accepted only as legacy input and upgraded to the adaptive count with a warning. Stable address hashing and write-if-changed output keep unrelated shards untouched when a reviewed seed is added. BIOS output remains one small translation unit. |

With an executable AOT scan range, discovery also recognizes base-relative
Thumb callback tables. A reachable `LDR offset,[base,#slot]; ADD target,base,offset;
BL <BX-target veneer>` supplies each exact slot offset. A table base loaded by
reachable code qualifies only when slot zero and another consumed slot resolve
to distinct validated PUSH-LR entries inside that range. Only consumed slots
are inspected; neighboring tables are not scanned for an end. This handles
negative as well as positive table-relative offsets.

### `[identity]` (required)

| key    | type   | meaning |
|--------|--------|---------|
| `sha1` | string | Required. Recompiler computes the binary's SHA-1 and aborts with a diagnostic if it disagrees. Anchors the TOML to a specific binary revision. |
| `md5`  | string | Optional cross-check. Same hard-abort behavior on mismatch. |

### `[[extra_func]]` (zero or more)

Hand-declared function entry points the finder might miss
(indirect dispatch targets, jump-table-only entries, gaps in finder
coverage).

| key           | type    | required | meaning |
|---------------|---------|----------|---------|
| `addr`        | int hex | yes      | First byte of the function, or an interior resume address when `resume = true`. |
| `source_addr` | int hex | no       | Immutable ROM backing address for an observed relocated RAM entry. The source/runtime bias propagates through direct CFG edges, including placements that overlap a fixed `code_copy` range. |
| `mode`        | string  | yes      | `"arm"` or `"thumb"`. Must match the actual instruction encoding at `addr`. |
| `resume`      | bool    | no       | Default `false`. Mark an interior IRQ/SWI return PC so it is rolled into its containing function and emitted as a thin resume alias. |
| `name`        | string  | no       | Function name in generated C. Default: `afunc_AAAAAAAA` (ARM) or `tfunc_AAAAAAAA` (THUMB). |
| `note`        | string  | no       | Free-form documentation. Ignored by the recompiler. |

Use `resume = true` only after confirming that `addr` is an instruction inside
an already rooted function. The alias wrapper records the requested guest PC and
enters the containing generated function, whose resume prologue jumps to that
instruction. If no compatible containing function is found, the candidate stays
a standalone function so the gap remains visible instead of being silently
attached to the wrong host.

### `[[resume_range]]` (zero or more)

A bounded declaration for a reviewed function that has produced asynchronous
IRQ returns at multiple interior instructions. `start` is the real function
entry and `end` is its exclusive, aligned extent. The recompiler keeps the host
whole and emits resume aliases for every aligned instruction after `start`.
This avoids reactively listing one `[[extra_func]] resume = true` entry per
observed interrupt timing while remaining narrower than a whole-program policy.
Ranges are capped at 0x1000 bytes, must remain inside the program image or a
declared `[[code_copy]]` runtime span, and may not overlap `[[data_range]]` or
contain an `[[exclude_func]]` address.

### `[[thumb_alu_immediate_override]]` (zero or more)

Declares one reviewed, halfword-aligned THUMB data-processing instruction whose
decoded immediate operand may consult the optional game-owned
`g_runtime_thumb_alu_imm_override` callback. Code generation emits the runtime
chokepoint only at the exact listed PC. With no entries—the universal
default—all immediates remain compile-time literals and generated code has no
callback check or performance cost. The callback's null/reject path preserves
the original decoded value. Entries must lie in the program image or a declared
`[[code_copy]]` span and may not lie in `[[data_range]]`.

This is intended for narrow, opt-in LLE-derived enhancement constants, not for
general guest patching. A listing does not make an ARM instruction or a THUMB
load/store offset overrideable; only a matching THUMB ALU immediate reaches the
hook. Runtime-healed overlays remain generic and do not consume this static
per-game allowlist, so an opted-in site must be present in the static corpus.

**Deduplication against the finder:**

| Finder finds    | TOML declares     | Result |
|-----------------|-------------------|--------|
| `0x100 thumb`   | `0x100 thumb`     | One entry. Manual name (and `note`) win. |
| `0x100 thumb`   | (nothing)         | One entry from finder. |
| (nothing)       | `0x100 thumb`     | One entry from manual. |
| `0x100 thumb`   | `0x100 arm`       | **Error** — mode mismatch. Hand-resolve. |

### `[[mod_function_hook]]` (zero or more)

Declares one reviewed ARM or THUMB function entry that generated code may hand
to a trusted native mod callback. Codegen emits the guard only when the exact
`addr` + `mode` pair matches an emitted function root:

```cpp
if (gba_mod_function_entry(0x08003000u, 1, &g_cpu)) return;
```

Callbacks start disabled, may decline a call, and on handling one must author
the post-call `ArmCpuState` (including the caller return PC). The original body
then does not execute. Badly targeted entries are rejected structurally
(alignment, image/copy span, data-range, or exclude-function conflict). An
otherwise valid declaration that matches no static root is a hard codegen
error, preventing address drift from silently disabling a mod. Self-healed
and force-interpreted dynamic code use the separately trusted registration
allowlist described in `MOD_PACKAGES.md`; static ROM roots always require this
TOML declaration.

| key | type | required | meaning |
|-----|------|----------|---------|
| `addr` | int hex | yes | Exact function entry PC. ARM must be word-aligned; THUMB halfword-aligned. |
| `mode` | string | yes | `"arm"` or `"thumb"`; part of the allowlist identity. |
| `note` | string | no | Free-form review documentation. |

### `[[data_range]]` (zero or more)

Byte ranges the finder must **not** decode as code. Hard exclusion:
the finder refuses to walk into the range from any direction.

**If control flow lands in a `data_range`, the recompile is a hard
error.** Either the range is wrong (remove or shrink it) or the
finder is following a path it shouldn't (fix the finder, or add an
`[[exclude_func]]` for the bogus walk origin). One is wrong. The
error message names both the data range and the entering branch's
PC so the contradiction is unambiguous:

```
ERROR: control flow enters data_range
  data_range:    [0x080001A0, 0x080001C0)   "literal pool after thumb_init"
  entering from: B 0x080001A4 at PC=0x08000180 (afunc_08000160)
Either:
  - shrink/remove the data_range covering 0x080001A4, or
  - mark afunc_08000160 with [[exclude_func]] if it's a false-positive function.
```

The forcing function is intentional. Silent ignore (downgrade to
miss) lets stale TOML rot.

| key     | type    | required | meaning |
|---------|---------|----------|---------|
| `start` | int hex | yes      | First excluded byte. |
| `end`   | int hex | yes      | One past last excluded byte (`[start, end)`). |
| `note`  | string  | no       | Free-form documentation. |

**Structural validation:**

- `start < end`.
- No overlap with any `[[extra_func]]` address.
- No overlap with any `[[jump_table]]`'s table bytes.
- Ranges may overlap each other (idempotent union).

### `[[code_copy]]` (zero or more)

Runtime code-copy mappings. Some GBA binaries copy executable bytes
from ROM into RAM and branch to the RAM address. This section tells
the finder how to decode a runtime address by reading bytes from its
ROM source address.

`[[code_copy]]` does **not** create function entries by itself. Add
`[[extra_func]]` entries for known runtime entry points, or let the
finder discover branches into the copied range. When decoding a
function in the copied range, generated code keeps the runtime PC
addresses while reading opcodes from the source bytes.

| key             | type    | required | meaning |
|-----------------|---------|----------|---------|
| `runtime_start` | int hex | yes      | First executable address after the copy completes. |
| `source_start`  | int hex | yes      | First source byte in the original binary. |
| `size`          | int hex | yes      | Number of copied bytes. |
| `name`          | string  | no       | Mapping label (documentation). |
| `note`          | string  | no       | Free-form documentation. |

The recompiler also emits position-aware native fragments for discovered
address-taken code and ROM-backed copies, including their direct branch and
fall-through continuations. These fragments support helpers copied onto a
moving EWRAM or IWRAM stack without a fixed `runtime_start` hint. They use the
live RAM PC for operands, literal alignment, links, branches, IRQ resumes and
fetch timing. The runtime compares the complete fragment against raw RAM before
entry and checks each instruction again before execution. Unsupported changes
return to normal dispatch and remain a loud strict-static miss unless
another AOT fragment matches. Matching has no bus-read or timing side effects;
it never decodes or compiles code at runtime. No additional configuration is
required. On Matrix cartridges, the active banked aperture uses the same byte
verification with cartridge fetch timing and live PC placement. Mapped pages can
be noncontiguous. Independently linked siblings of a known copied startup module
are discovered from its exact ARM mode/MSR prefix with separate source mappings;
these additional bodies enter the guarded catalog. Unknown bytes cannot fall
back to a stale fixed-address native
entry. Reviewed mod hooks and immediate overrides still require their exact
live guest PC. BIOS compilation remains separate.

Copied ARM code can initialize immediate operands in place. For unconditional
`MOV` and `ADD` without flag updates or PC operands/destinations, the native
body can read the immediate field as live data. The opcode, condition,
registers, and flag behavior remain fixed and guarded, including on cached
entries and instruction resumes. Other fields and instruction forms still
require an exact match. This is a fixed native operation with a data parameter;
the runtime does not decode an instruction or choose an operation.

The finder also recognizes an exact `LDR word; CMP word,constant; STREQ`
compare-and-replace initializer rooted in ROM-backed RAM. A same-address store
of a known ARM `STR` opcode can produce a second, exact initialized native
variant. Its span follows the copied routine's non-call control flow through
split loop/resume labels. Conditional stores with another predicate, changed
addresses, unknown replacement words, and control-flow replacements do not
qualify. Matrix module section descriptors additionally map initialized RAM
sections with checked source, destination, and size bounds.

ARM indexed word tables support backward PC-relative address formation and
ROM-backed RAM. A sparse table immediately before a known PUSH-LR dispatcher
can include null slots only after its entire span is checked as null or valid
code pointers. The dispatcher entry supplies the end of that span. Thumb byte
and halfword relative tables continue to require a dominating CMP range bound.

`GBARECOMP_STRICT_STATIC=1` rejects explicit interpreter bridges and forced
interpreter hooks as well as dispatch misses. Coverage counts forced and direct
bridge instructions; zero misses alone does not imply `FULLY_STATIC`.

Callback fields in record arrays are also recovered when a reachable dispatcher
forms their exact stride from an unsigned byte index, loads the selected field
and branches through it. Discovery checks all 256 possible byte values, accepts
only validated function prologues, and skips null or non-code slots. It does not
guess a record count from a sentinel or classify the other fields as code.

### `[[jump_table]]` (zero or more)

Annotated dispatch tables. The finder reads `count` entries of
`stride` bytes starting at `addr`, decodes each entry according to
`format`, and registers each decoded target as an `extra_func`
equivalent.

The bytes of the table itself are **automatically excluded from
code decoding** (no need to also declare a `[[data_range]]`
covering them).

| key            | type    | required | meaning |
|----------------|---------|----------|---------|
| `addr`         | int hex | yes      | First byte of the table. |
| `stride`       | int     | yes      | Bytes per entry. Typical: 4. |
| `count`        | int     | yes      | Number of entries. |
| `format`       | string  | yes      | `abs32`, `abs16`, `pcrel_thumb`, `pcrel_arm`, `u8_rel_thumb`, or `u16_rel_thumb`. |
| `base`         | int hex | relative Thumb only | Address of the consuming `ADD pc,Rt` instruction + 4. |
| `entries_mode` | string  | yes      | `"arm"`, `"thumb"`, or `"auto"`. See below. |
| `name`         | string  | no       | Table label (documentation). |
| `note`         | string  | no       | Free-form. |

Format semantics:

- `abs32` — `uint32_t` per entry; value is the target address.
- `abs16` — `uint16_t` per entry; value is the target address. Rare on GBA outside small ranges.
- `pcrel_thumb` — `int32_t` per entry; target =
  `(entry_address + value) & ~1`, decoded as THUMB. The low interworking
  bit is normalized out of the instruction address.
- `pcrel_arm` — `int32_t` per entry; target = `entry_address + value`, decoded as ARM.
- `u8_rel_thumb` / `u16_rel_thumb` — unsigned byte / halfword entries;
  target = `base + 2 * value`, decoded as THUMB. `stride` must be 1 / 2,
  respectively; halfword tables must be aligned. `count` must be nonzero.
  `base` is the Thumb PC value at `ADD pc,Rt`, **without word alignment**.
  These targets become case blocks and resume entries of the dispatcher host.
  Discovery recognizes the bounded `ADR` or literal `LDR`, indexed `LDRB` /
  `LDRH`, `LSLS #1`, `ADD pc` idiom automatically. Manual entries supply the
  exact table count when its guard cannot be recovered; they are consumed at
  `base - 4`, which must be a reachable Thumb `ADD pc,Rt` instruction.

```toml
[[jump_table]]
addr = 0x080014B8
base = 0x080014BA # ADD pc,r3 at 0x080014B6 reads PC + 4
stride = 1
count = 7
format = "u8_rel_thumb"
entries_mode = "thumb"
```

`entries_mode` semantics:

- `"arm"` / `"thumb"` — every entry's target is decoded in the
  declared mode. The low bit of the entry value (where applicable)
  is **not** consulted.
- `"auto"` — per-entry mode is decoded from bit 0 of the entry
  value: bit 0 set → THUMB, bit 0 clear → ARM. The bit is then
  cleared to recover the actual target address.

`"auto"` exists because the GBA's ARM/THUMB interworking convention
(`BX Rn` / `BLX Rn`) encodes mode in bit 0 of the target register,
so compiler-emitted jump tables fed to `BX` are typically
mixed-mode: one table contains entries pointing into both ARM and
THUMB code. Forcing a single `entries_mode` for the whole table
would mis-decode every entry on the other side.

Mode-keyword combinations:

| `format`        | valid `entries_mode`              |
|-----------------|-----------------------------------|
| `abs32`         | `"arm"`, `"thumb"`, `"auto"`      |
| `abs16`         | `"arm"`, `"thumb"`                |
| `pcrel_arm`     | `"arm"` (must match format)       |
| `pcrel_thumb`   | `"thumb"` (must match format)     |
| `u8_rel_thumb`  | `"thumb"` (must match format)     |
| `u16_rel_thumb` | `"thumb"` (must match format)     |

`abs16` does not support `"auto"`: 16-bit pointers don't span an
interworking address space on the GBA.

The pcrel formats have mode baked into the keyword, so
`entries_mode` must match — any other value is a structural error.
This redundancy is intentional: the schema reader doesn't have to
disambiguate, and the TOML stays self-documenting.

### `[[exclude_func]]` (zero or more)

Escape valve: if the finder produces a false-positive entry the
TOML author can't predict structurally (e.g., a corrupted heuristic
walk), name the address here to remove it from the dispatch table.

| key      | type    | required | meaning |
|----------|---------|----------|---------|
| `addr`   | int hex | yes      | The false-positive entry to remove. |
| `reason` | string  | yes      | Required — forces the author to articulate why. |

`[[exclude_func]]` **takes precedence over `[[extra_func]]`** when
they conflict; declaring both at the same address is a hard error
(contradictory intent).

## Overlay configs (`--config` more than once)

`--config` is repeatable. The **first** path is the base config described
above: it owns `[program]` and `[identity]`, and its hash is verified against
the actual bytes before anything else runs. Every **later** path is an
*overlay* — a supplemental config merged on top of the base.

An overlay exists so that machine-generated configuration can compose with a
hand-authored `game.toml` instead of replacing it. The generated decomp symbol
overlay (see `SYMBOL_OVERLAY.md`) is the motivating case: it contributes
hundreds of `[[data_range]]` entries derived from the decomp's own link
layout, while the game's reviewed `[[extra_func]]`, `[[code_copy]]`,
`[[jump_table]]` and immediate-override entries — each carrying a `note` that
records the evidence for it — stay exactly where they are. No tool ever writes
`game.toml`.

Rules:

| Rule | Behaviour |
|---|---|
| `[program]` in an overlay | **abort.** Only the base may define the program. |
| `[identity]` in an overlay | optional; when present its `sha1` **must equal the base's**, else abort. This binds a generated overlay to the exact binary it was derived from. |
| `[[extra_func]]` at an (addr, mode) the base already declares | overlay entry **dropped**; the base keeps its name and note. |
| overlay `[[extra_func]]` the base `[[exclude_func]]`s | dropped, with a warning naming the base's exclusion reason. |
| overlay `[[extra_func]]` inside a base `[[data_range]]` | dropped, with a warning. |
| base `[[extra_func]]` inside an overlay `[[data_range]]` | **abort** (ordinary structural validation). Reviewed evidence and the decomp's link layout genuinely disagree; resolve it rather than suppress it. |
| duplicate `[[data_range]]` / `[[code_copy]]` / `[[jump_table]]` / `[[exclude_func]]` | redundant duplicate dropped, base's copy kept. |
| everything else | appended. |

The base always wins, which is the same precedence the function finder applies
downstream: base seeds are registered first and the first seed for an
(addr, mode) supplies the name.

Structural validation re-runs over the merged result, so an overlay cannot
smuggle in a contradiction the base alone would have rejected.

Each overlay prints its own summary:

```
[gba_recompile overlay: symbols\AMKE_symbols.toml]
  extra_func:  +0 (base wins 0, excluded 0, in base data_range 0)
  data_range:  +132   code_copy: +0   jump_table: +0
```

## Precedence and conflict resolution

In order of evaluation:

0. **Overlay merge** — later `--config` files folded into the base as above.
1. **Identity check** — SHA-1 (and optional MD5) verified against the binary. Mismatch → abort.
2. **Structural validation** — overlapping `extra_func` + `data_range`, mode disagreements within the TOML, `exclude_func` colliding with `extra_func`. Any → abort with diagnostic.
3. **Data ranges** registered. Finder will refuse to walk into them.
4. **Code-copy ranges** registered. Finder can decode runtime RAM addresses from their ROM source bytes.
5. **Jump tables** read, expanded to per-target `extra_func` equivalents.
6. **`extra_func` entries** registered (including the entry from `[program].entry_pc`).
7. **Finder walks** from the union of seeds. Every walked target compared against:
   - existing `extra_func` registry → dedupe (mode mismatch → error)
   - `data_range` → walk stops at boundary
8. **`exclude_func`** entries applied last — remove from final dispatch table.
9. **Summary printed** (see below).

## Summary output

After every `gba_recompile` run with a config:

```
[gba_recompile config: F:\Projects\gbarecomp\gbarecomp\bios\gba_bios.toml]
  identity sha1:           300c20df... (verified)
  extra_func entries:      54
  data_range entries:      12
  code_copy entries:       1
  jump_table entries:      1   (expanded to 43 extra_func equivalents)
  exclude_func entries:    0

[gba_recompile discovery summary]
  discovered_by_walk:      37     finder-only
  redundant_manual:        37     in both — TOML carries documentation value
  manual_seeds_only:       60     would be lost without TOML
  finder_rejected_walks:   2      hit a data_range boundary
  code_copies:             1
  excluded:                0
  TOTAL emitted:           97
```

The metric of finder quality is the trend of `manual_seeds_only`
downward over time, with `redundant_manual` rising proportionally
(or staying flat as the TOML grows for new use cases). `finder_rejected_walks`
counts how often a `data_range` saved us from a false positive.

## Runtime video settings

The runtime also consumes an optional `[video]` table. These keys do not affect
static discovery or generated code:

```toml
[video]
screen = "frontlit"
view_width = 240
```

`view_width` is the total logical output width in GBA pixels. Height remains
160. `240` is faithful and is the universal default. Values above 240 are
honored only when the game runner advertises an equal-or-larger
`RunOptions::max_view_width`; otherwise the runtime clamps to 240. Command-line
`--view-width` and environment `GBARECOMP_VIEW_WIDTH` override TOML in that
order. Legacy `widescreen=N` inputs map to `240 + 2*N` immediately and are kept
only for compatibility.

## Hash-anchored

Every TOML is bound to a specific binary by `[identity].sha1`. If
the binary changes (revision, region, patch), a new TOML is
authored. Mixing TOMLs across revisions is a hard error, not a
warning.

## What this schema deliberately does NOT support

- **`if`-conditional entries** (per-region, per-revision dynamic) —
  one TOML per binary. Duplicate files share via include or a
  generator script, not in-file conditionals.
- **Wildcard / pattern matches** — every entry is a single address
  or a closed range. No globs.
- **Per-entry attributes for the runtime** — calling convention,
  cycle cost, side effects, etc. Those belong in code, not config.
- **Comment fields that affect behavior** — `note` and `reason` are
  for humans only. Removing them never changes recompiler output.

## Convention compatibility

Naming follows the sibling-recompiler convention:

- `[[extra_func]]` — matches `nesrecomp`, `segagenesisrecomp`, `snesrecomp`.
- `[[data_range]]` — matches `nesrecomp` (`data_region`) and
  `gbcrecomp` (`[[data_region]]`). We use `data_range` for the
  byte-range semantics; `data_region` in the sibling projects is a
  superset that also names the region.
- `[[jump_table]]` — matches `segagenesisrecomp`.
- `[[exclude_func]]` — new (segagenesisrecomp uses `blacklist`; this
  name is clearer).

A future port of the TOML schema across recompilers should be
straightforward.
