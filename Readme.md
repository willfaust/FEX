> **Licensing note for this fork.** Upstream code here remains under its
> original licence (see `LICENSE`). Modifications and new files authored for
> [Madeira](https://github.com/willfaust/Madeira) are **GPL-3.0-or-later**
> (`COPYING.GPL-3.0`), so this copy as a whole is copyleft. Upstream remains
> available under its own licence from upstream. See `LICENSE-MADEIRA.md`.

[中文](https://github.com/FEX-Emu/FEX/blob/main/docs/Readme_CN.md)
# FEX: Emulate x86 Programs on ARM64
FEX allows you to run x86 applications on ARM64 Linux devices, similar to qemu-user and box64.
It offers broad compatibility with both 32-bit and 64-bit binaries, and it can be used alongside Wine/Proton to play Windows games.

It supports forwarding API calls to host system libraries like OpenGL or Vulkan to reduce emulation overhead.
An experimental code cache helps minimize in-game stuttering as much as possible.
Furthermore, a per-app configuration system allows tweaking performance per game, e.g. by skipping costly memory model emulation.
We also provide a user-friendly FEXConfig GUI to explore and change these settings.

## Building for Madeira on iOS

This branch also contains Madeira's iOS port. The Linux setup guide linked below
builds the Linux emulator, not the components used by Madeira.

For 64-bit games, Madeira uses two different FEX builds:

- Native ARM64 static libraries linked into the iOS app.
- A Windows ARM64EC DLL loaded by Wine on iOS, packaged as `xtajit64.dll`.

These targets need different SDKs, compilers and build directories. An ordinary
Linux configure command does not select either target. On macOS, its default
CPU probes also try to read Linux's `/proc/cpuinfo`. The wrappers below select
an explicit CPU target and avoid those probes.

Use the FEX revision pinned by a [Madeira checkout](https://github.com/willfaust/Madeira),
including its nested submodules. On macOS, install full Xcode with the iOS SDK,
Homebrew, Git and Python 3.9 or later. Run these commands from the **Madeira
repository root**, not from the FEX directory:

```sh
# Check that the selected Xcode provides the iPhoneOS SDK.
xcodebuild -version
xcrun --sdk iphoneos --show-sdk-path

brew install cmake ninja
git submodule update --init --recursive FEX

# Native iOS libraries: FEX/build-ios/.
bash tools/build/build-fex-ios.sh

# ARM64EC module: FEX/build-arm64ec/, then the app's xtajit64.dll.
# The setup script downloads the pinned compiler only when it is absent.
bash tools/build/setup-llvm-mingw.sh
bash build/fex-arm64ec/build.sh
```

Keep the build directories separate. The native build uses the iPhoneOS SDK;
the ARM64EC build uses the pinned llvm-mingw toolchain and the iOS-host flags
in the wrapper. Do not add `FEX_IOS_HOST` to the native library build: that
macro selects the Windows module's Wine/iOS bridge.

For 32-bit games, Madeira also needs the separate WoW64 module. See
[Madeira's WoW64 build instructions](https://github.com/willfaust/Madeira/blob/main/docs/WOW64.md).
For the complete app and IPA, use
[Madeira's build guide](https://github.com/willfaust/Madeira/blob/main/BUILDING.md).

If a component build fails, include the failing command, the first error and
its surrounding output, the Madeira and FEX commit IDs, host OS/CPU, and Xcode,
CMake and compiler versions. The last `ninja: build stopped` line alone does
not identify the cause.

## Linux prerequisites
FEX requires ARMv8.0+ hardware. It has been tested with the following Linux distributions, though others are likely to work as well:

- Arch Linux
- Fedora Linux
- openSUSE
- Ubuntu 22.04/24.04/24.10/25.04

An x86-64 RootFS is required and can be downloaded using our `FEXRootFSFetcher` tool for many distributions.
For other distributions you will need to generate your own RootFS (our [wiki page](https://wiki.fex-emu.com/index.php/Development:Setting_up_RootFS) might help).

## Linux quick start
### For Ubuntu 22.04, 24.04, 24.10 and 25.04
Execute the following command in the terminal to install FEX through a PPA.

```sh
curl --silent https://raw.githubusercontent.com/FEX-Emu/FEX/main/Scripts/InstallFEX.py | python3
```

This command will walk you through installing FEX through a PPA, and downloading a RootFS for use with FEX.

### For other Distributions
Follow the guide on the official FEX-Emu Wiki [here](https://wiki.fex-emu.com/index.php/Development:Setting_up_FEX).

### Navigating the Source
See the [Source Outline](docs/SourceOutline.md) for more information.
