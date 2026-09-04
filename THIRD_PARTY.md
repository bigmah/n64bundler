# Third-party components

Everything N64Bundler builds on, with its license. No game data is included in
any of it; see the legal notice in [`README.md`](README.md).

## Pinned as submodules

| Component | Where | License |
|---|---|---|
| [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) — `ultramodern` and `librecomp` | `ModernReality/vendor/N64ModernRuntime` | MIT |
| [N64Recomp](https://github.com/N64Recomp/N64Recomp) — the recompiler | `ModernReality/vendor/N64ModernRuntime/N64Recomp` | MIT |
| [RT64](https://github.com/rt64/rt64) — the renderer | `ModernReality/vendor/rt64` | MIT |

Both upstreams carry their own third-party trees, which come down with them:

| Component | Used by | License |
|---|---|---|
| [rabbitizer](https://github.com/Decompollaborate/rabbitizer) | N64Recomp, and `n64rip` for decoding | MIT |
| [ELFIO](https://github.com/serge1/ELFIO) | N64Recomp | MIT |
| [fmt](https://github.com/fmtlib/fmt) | N64Recomp | MIT |
| [toml++](https://github.com/marzer/tomlplusplus) | N64Recomp | MIT |
| [sljit](https://github.com/zherczeg/sljit) | N64Recomp's live recompiler | BSD-2-Clause |
| [xxHash](https://github.com/Cyan4973/xxHash) | N64ModernRuntime, RT64 | BSD-2-Clause |
| [miniz](https://github.com/richgel999/miniz) | librecomp | MIT |
| [o1heap](https://github.com/N64Recomp/o1heap) | librecomp | MIT |
| [concurrentqueue](https://github.com/cameron314/concurrentqueue) | ultramodern | BSD-2-Clause / Boost |
| [nlohmann/json](https://github.com/nlohmann/json) | N64ModernRuntime | MIT |
| [sse2neon](https://github.com/DLTcollab/sse2neon) | librecomp's RSP vector unit, on arm64 | MIT |
| [Dear ImGui](https://github.com/ocornut/imgui) | RT64 | MIT |
| [ImPlot](https://github.com/epezent/implot) | RT64 | MIT |
| [hlslpp](https://github.com/redorav/hlslpp) | RT64 | MIT |
| [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) | RT64's Metal shaders | Apache-2.0 |
| [DirectXShaderCompiler](https://github.com/rt64/dxc-bin) | RT64's shader build | LLVM / NCSA |
| [stb](https://github.com/nothings/stb) | RT64 | MIT / public domain |
| [zstd](https://github.com/facebook/zstd) | RT64 | BSD-3-Clause / GPL-2.0 |
| [ddspp](https://github.com/redorav/ddspp) | RT64 | MIT |
| [im3d](https://github.com/john-chapman/im3d) | RT64 | MIT |
| [nativefiledialog-extended](https://github.com/btzy/nativefiledialog-extended) | RT64 | Zlib |
| [metal-cpp](https://developer.apple.com/metal/cpp/) | RT64's Metal backend | Apache-2.0 |
| [SDL2](https://www.libsdl.org/) | RT64 and `n64b-run`'s window, audio and controllers; linked from Homebrew | Zlib |

## Changes carried against upstream

Kept as patches under [`N64Bundler/patches/`](N64Bundler/patches) and applied
by `build.sh`. When there are enough of them to be worth pushing back, they
become forks and this becomes a `forks.sh`, the way DolBundler's did.

All three exist for the same reason: the recompiler assumes its symbols came
from an elf, where anything missing means the symbols are wrong. Recovered
symbols are incomplete by nature, and the difference has to be tolerated rather
than treated as a defect. None of them changes what a project recompiling from
an elf sees.

`0001-n64recomp-recompile-from-recovered-symbols.patch`:

- **Unresolvable calls fall back to runtime lookup.** A `jal` to an address no
  section covers currently fails the build. From a bare ROM it just means the
  call leaves the code the analysis found — an overlay, in practice. A new
  `lookup_unresolved_function_calls` option routes those through the same
  runtime function lookup an indirect call to the same address would use. Off
  by default.

- **Declare every function whose body is not emitted, not only the
  reimplemented ones.** An ignored libultra function is still called from the
  recompiled code, so without a declaration the output does not compile under
  any C standard since C99. A project driving the recompiler by hand writes
  that declaration itself; one built from recovered symbols has nowhere to
  write it.

- **A write to `$zero` is a discard, not an assignment.** `lw $zero, 0(a0)` is
  a real idiom — a load kept for its side effect on the cache or the load slot.
  The generator renders `$zero` as the literal `0`, so the output was
  `0 = MEM_W(...)`, which no C compiler accepts. It now emits `(void)(...)`,
  which is what the instruction does.

## License of this repository

GPL-3.0-or-later. Every upstream above is permissively licensed and compatible
with that.
