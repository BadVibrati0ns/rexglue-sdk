> [!NOTE]
> **This is a fork of [rexglue/rexglue-sdk](https://github.com/rexglue/rexglue-sdk)** with
> performance work for the emulated (Xenos) GPU path, meant to help any Xbox 360 game built on
> ReXGlue. The `patched` branch is upstream **v0.10.0** plus the changes below. Everything else
> in this README is upstream's.
>
> **Credit.** The emulated GPU work in this fork is the SDK patch from
> **[Proving Ground Recompiled](https://github.com/Newbie1337x/ProvingGroundRecomp)** by
> **Tomas cabrera ([Newbie1337x](https://github.com/Newbie1337x))**:
> `patches/rexglue-sdk.patch` at
> [`4e884d0`](https://github.com/Newbie1337x/ProvingGroundRecomp/commit/4e884d0383c64d6d005d21fe4d3f60daa93c1c78),
> applied unchanged as commit `37a3b0c`, which carries its author's name. Per that project's
> license, the patch is under this SDK's BSD 3-Clause license. It includes a vblank deadline
> wait adapted from the **Tony Hawk's Project 8 recompilation project** (Project8Recomp),
> BSD 3-Clause; its license is in [`LICENSES/`](LICENSES/Project8Recomp-BSD-3-Clause.txt).
> Thank you to both projects.
>
> **What the patch changes** (all of it behind options, most on by default):
>
> - Command processor: a known-register bitset instead of a register-info lookup on every
>   register write, and type-0 packets written as ranges (`gpu_type0_register_ranges`).
> - Vulkan: submission on a second thread (`vulkan_async_submission`), descriptor-set reuse,
>   merged draw binds, dynamic constant buffers, cached sampler parameters and texture lookups.
> - Shared memory: pages the CPU keeps rewriting are compared instead of write-protected
>   (`shared_memory_hot_page_uploads`), upload windows, fewer redundant re-protections, and
>   invalidations that stop at texture watches.
> - Render targets: transfers skipped when a full-screen rectangle overwrites them.
> - Vblank pacing options (`vblank_after_swap`, a deadline wait), performance counters and
>   timelines, and FSR 3.1 / FSR 4 presenter paths (FidelityFX, optional at build time).
>
> **Measured** in a Tony Hawk's Proving Ground recompilation (same input replay over the same
> stretch of gameplay, emulated renderer at 720p internal, RTX 4090 Laptop, Linux): 50.6 fps on
> stock v0.10.0, 154.3 fps on this branch.
>
> **Further fixes** in this fork, each its own commit, moved from host-side workarounds in the
> Tony Hawk's Proving Ground recompilation (THPG-decomp) into the SDK source:
>
> - POSIX waits sleep to a deadline at full precision: stock truncated the remaining time to
>   whole milliseconds, so alertable waits spun for their whole length (20 alertable 5 ms
>   waits: 99.3 ms of CPU before, 0.6 ms after).
> - The timer queue's dispatch thread sleeps until a timer is due or a new one is queued,
>   instead of spinning in disruptorplus's `spin_wait_strategy` (3.1% of a core with one 16 ms
>   timer before, 0.06% after).
>
> Changes that benefit Xbox 360 games in general are added here; game-specific work stays in
> each game's own project.

> [!CAUTION]
> This project is in early development. Expect things to not work quite right and there to be significant changes and breaking public API updates as development progresses. Contributions and feedback are welcome, but please be aware that the codebase is still evolving rapidly.

<h1 align="center">
  <br>
  <a href="https://github.com/rexglue/rexglue-sdk">
    <img src="https://github.com/rexglue/rexglue-media/blob/main/ReX_Banner.png" alt="ReXGlue banner">
  </a>
  <br>
  <br>
  <a href="https://discord.gg/CNTxwSNZfT">
    <img src="https://img.shields.io/badge/Discord-Join%20Server-5865F2?logo=discord&logoColor=white" alt="Discord">
  </a>
  <a href="https://github.com/rexglue/rexglue-sdk/stargazers">
    <img src="https://img.shields.io/github/stars/rexglue/rexglue-sdk" alt="rexglue-sdk stargazers">
  </a>
</h1>

ReXGlue converts Xbox 360 PowerPC code into portable C++ that runs natively on modern platforms.

ReXGlue is heavily rooted on the foundations of [Xenia](https://github.com/xenia-project), the Xbox 360 emulator. Rather than interpreting or JIT-compiling PPC instructions at runtime, ReXGlue takes a different path: it generates C++ source code ahead of time, an approach inspired by [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and [rexdex's recompiler](https://github.com/rexdex/recompiler).

Latest SDK builds and releases are published on [GitHub Releases](https://github.com/rexglue/rexglue-sdk/releases). Join the [Discord server](https://discord.gg/CNTxwSNZfT) for updates and share what you have created.

## Builds

| Channel | CI | Download |
| --- | --- | --- |
| Release | [![win-amd64](https://github.com/rexglue/rexglue-sdk/actions/workflows/build-win-amd64.yaml/badge.svg)](https://github.com/rexglue/rexglue-sdk/actions/workflows/build-win-amd64.yaml) [![linux-amd64](https://github.com/rexglue/rexglue-sdk/actions/workflows/build-linux-amd64.yaml/badge.svg)](https://github.com/rexglue/rexglue-sdk/actions/workflows/build-linux-amd64.yaml) [![linux-arm64](https://github.com/rexglue/rexglue-sdk/actions/workflows/build-linux-aarch64.yaml/badge.svg)](https://github.com/rexglue/rexglue-sdk/actions/workflows/build-linux-aarch64.yaml) | [Latest stable](https://github.com/rexglue/rexglue-sdk/releases/latest) |
| Nightly | [![nightly](https://github.com/rexglue/rexglue-sdk/actions/workflows/nightly.yaml/badge.svg)](https://github.com/rexglue/rexglue-sdk/actions/workflows/nightly.yaml) | [Latest pre-release](https://github.com/rexglue/rexglue-sdk/releases?q=prerelease%3Atrue) |

## Quickstart

For quick start guide, full CLI reference, and config file options, see the [wiki](https://github.com/rexglue/rexglue-sdk/wiki).

# **Disclaimer**
ReXGlue is not affiliated with nor endorsed by Microsoft or Xbox. It is an independent project created for educational and development purposes. All trademarks and copyrights belong to their respective owners. 

This project is not intended to promote piracy nor unauthorized use of copyrighted material. Any misuse of this software to endorse or enable this type of activity is strictly prohibited.


# Credits

## ReXGlue
- [Tom (crack)](https://github.com/tomcl7) - Project Founder
- [Loreaxe](https://github.com/Loreaxe) - Linux Contributor
- [mystixor](https://github.com/Mystixor) - Windows Contributor
- [Graine25](https://github.com/Graine25) - Project Support
- [Carlos Estrague (mrcmunir)](https://github.com/mrcmunir) - Linux / ARM64 Contributor
- [sanjay900](https://github.com/sanjay900) - Linux / SDL Contributor
- [Toby](https://github.com/TbyDtch) - Project Support
- [Roxxsen](https://github.com/Roxxsen) - CI/CD Contributor

The list above is not exhaustive. Thanks to everyone in the ReXGlue community who contributes code, files issues, tests builds, and keeps the project moving.

## Very Special Thank You:
- [Project Xenia](https://github.com/xenia-project/xenia/tree/master/src/xenia) - Their invaluable work on Xbox 360 emulation laid the groundwork for ReXGlue's development. This project (and numerous others) would not exist without their hard work and dedication.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) - For pioneering the modern static recompilation approach for Xbox 360. A lot of the codegen analysis logic and instruction translations are based on their work. Thank you!
- [rexdex's recompiler](https://github.com/rexdex/recompiler) - The OG static recompiler for Xbox 360. 
- Many others in the Xbox 360 homebrew and modding communities whose work and research have contributed to the collective knowledge that makes projects like this possible.
