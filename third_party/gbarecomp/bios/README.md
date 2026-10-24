# BIOS

Drop your own dump of the GBA BIOS here as `gba_bios.bin`. The binary
**is not in git** (it's copyrighted Nintendo code) but the `.toml` /
`.md` / `.sym` files in this folder ARE tracked, so the path layout
matches between developer machines.

## Maintain the real BIOS reference

The LLE configuration statically recompiles and executes the actual BIOS.
Keep it buildable and functioning as the accuracy reference for SWIs, IRQs,
boot and their required device effects. The interpreter can diagnose CPU
coverage gaps; independent emulators help diagnose shared hardware-model bugs.

Native BIOS service implementations are allowed under [HLE.md](../HLE.md).
They preserve the supported caller contract and may approximate internal work
and timing. The existing legacy BIOS selectors do not replace the build-time
selection policy for new services. Boot-intro playback is a separate option.
Never use a success stub to conceal a missing required effect or LLE route.

## Required image

| Field    | Value                                                                 |
|----------|-----------------------------------------------------------------------|
| File     | `bios/gba_bios.bin`                                                   |
| Size     | 16384 bytes (16 KB)                                                   |
| SHA-1    | `300c20df6731a33952ded8c436f7f186d25d3492`                            |
| MD5      | `a860e8c0b6d573d191e4ec7db1b1e4f6`                                    |
| Origin   | Nintendo GBA system ROM (region-agnostic)                             |

The runtime refuses to start with a BIOS whose hash isn't recognized.
This is the same gate the cartridge ROM goes through; both must
verify.

## Where to put it

`bios/gba_bios.bin`

The `bios_smoke` tool, the runtime, and `gba_recompile` all look here
by default. Override with `--bios <path>` if you keep yours elsewhere.

## What lives here that IS tracked

- `README.md` — this file.
- `gba_bios.toml` — audit / recompile config for the BIOS, matching
  the shape of a game's `game.toml`.
- `gba_bios.sym` — symbol map (when populated). Sources are public
  GBA BIOS disassemblies; see `../docs/GBA_REFERENCE_NOTES.md`
  "GBA BIOS references."

## What does NOT live here

- Owned BIOS bytes or ROM-derived generated sources in version control.
- Decompiled pseudo-C treated as an independent execution oracle.
- Success stubs that omit required BIOS behavior.

Native service implementations live in their appropriate runtime component,
with licensing, caller contracts and a maintained LLE build documented.

Generate locally with `gba_recompile --bios bios/gba_bios.bin --config
bios/gba_bios.toml` into the ignored `src/runtime/generated_bios` directory,
or configure an explicit `GBARECOMP_GENERATED_BIOS_DIR`. Generated source
is a private build input and never enters version control.
