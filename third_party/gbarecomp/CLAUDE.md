# GBA agent entry point

Read [PRINCIPLES.md](PRINCIPLES.md), [DEBUG.md](DEBUG.md) and [TCP.md](TCP.md).
The shared [recomp-template principles](../../recomp-template/PRINCIPLES.md)
remain authoritative. For HLE, LLE or performance work, read [HLE.md](HLE.md)
and [the shared optimization policy](../../recomp-template/OPTIMIZATION.md).

## Implementation boundaries

- Fix codegen, runtime, hardware models or reviewed game metadata, then
  regenerate. Do not hand-edit ROM-derived generated C/C++.
- Maintain a functioning LLE build for every HLE-replaced service. A qualified
  native HLE build may be preferred. Select implementations at build time.
- HLE may replace whole services, batch work, use private state and approximate
  internal timing under the supported caller contract. Do not require live
  switching, continuous dual execution or cross-build savestate compatibility.
- Preserve netplay determinism. Presentation can vary only when it cannot feed
  guest readback, scheduling, state hashes or rollback. Settle qualified CPU and
  backend capabilities once before frame 0.
- Gate direct assembly/SIMD and their dependencies by CPU/OS/ABI. Keep a working
  portable implementation on every supported target.

## Build and coverage

Build `gba_recompile`, regenerate from the verified ROM/BIOS plus reviewed
configuration, then build the title. Use the actual BIOS generation config and
document which implementations the executable selects. Exact native CPU/LLE
work uses instruction and state comparisons; deliberate HLE compares its
documented inputs, outputs, effects and acceptable differences.

After runs, inspect coverage and dispatch misses. Interpreted or healed code is
reported separately from native AOT and intentional HLE; do not call incomplete
CPU coverage fully static. Keep recovery loud and retain reproducible evidence.
Review seed proposals before changing game metadata. Correctness fixes work by
default; elective presentation enhancements remain optional.

## Investigation and delivery

Find the first violation of the promised contract. Validate instruments and
distinguish missing behavior from a wrong result. Diagnostic payload recording
may be gated in normal builds; retain useful counters and diagnostic tooling.
Use focused checks and bounded measurements, then normal-audio owner QA.
Automated/background game runs must remain muted. Respect the owner's central
Beads hierarchy and solo-work policy. Never ship owned inputs or ROM-derived
generated sources. Keep unrelated working-tree changes intact.

## Player bug reports

Recorded diagnostic replays can reproduce a player's report. Use
`tools/replay_diagnostic.py replay <first .replay> --exe ... --rom ... --bios ...`
to stop at the first divergent frame or re-execute the frame that hung/crashed.
See [docs/DIAGNOSTIC_REPLAY.md](docs/DIAGNOSTIC_REPLAY.md) for capture defaults
and controls. Preserve the supplied recordings before investigating.
