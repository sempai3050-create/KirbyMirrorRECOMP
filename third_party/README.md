# Pinned native dependencies

The Android port uses the shared GBARecomp Android runtime and its required
source dependencies. These upstream sources are kept at the exact revisions
used by the Android reference project:

- `gbarecomp`: `ec778f172395d3118e48dc0d4ce8e563790f86d6`
- `recomp-ui`: `0546fbcb0126bb14bf88d2128b4fe8a4229570a3`
- `gbarecomp/external/arm-recomp-core`:
  `61cdc51a85efe621841458d9e6c35c5357fcb2ee`
- `gbarecomp/external/rbengine`: `2a03e73693acee0fb78076ea058642c931bef12e`
- `gbarecomp/external/recomp-net`:
  `8c76fb643339144a3a84d5ba8f5defe6250dcd26`
- `gbarecomp/platform/android/third_party/SDL`:
  `8e28503b164d4353262ed5abf102c25fa838e4f8`
- `gbarecomp/third_party/tomlpp/toml.hpp`: toml++ v3.4.0 single-header
  distribution at commit `30172438cee64926dc41fdd9c11fb3ba5b2ba9de`

Each dependency's upstream license and notices are retained in its directory.
The Kirby ROM and GBA BIOS are not included.
