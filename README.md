# UNA Watch kernel

The firmware that runs on the UNA Watch. With it you can build the same kernel image that UNA Watch Ltd. releases, and install it on your watch.

Pull requests are welcome against `main`. To write apps for the watch, see the [UNA SDK](https://github.com/UNAWatch/una-sdk).

## What the build produces

| File | What it is | Install with |
| --- | --- | --- |
| `UnaWatch-Kernel_<version>.ota` | The kernel, encrypted for the watch's bootloader | The bootloader's USB drive, or the UNA app |
| `UnaWatch-Kernel_<version>.gld` | The same kernel, kept by the bootloader as the image to restore on a factory reset | The bootloader's USB drive |
| `UnaWatch-Kernel_<version>.hex` | Bootloader, shared C library and kernel together: the watch's whole flash | |

The `.ota` and `.gld` files are encrypted with the key in `una-bootloader/Software/Keys/SecurityKeys.pem`, which every UNA Watch's bootloader uses.

## Building

You need Docker and git. Clone the repository with its submodule, at the release you want:

```bash
git clone --recurse-submodules --branch v<version> https://github.com/UNAWatch/una-kernel-public.git
cd una-kernel-public
```

Then build it in the same container that UNA's releases are built in:

```bash
docker run --rm -v "$PWD:/workspace" xanderhendriks/stm32cubeide:16.0 bash -c \
    'apt-get update && apt-get install -y git && \
     git config --global --add safe.directory /workspace && \
     . /workspace/SDK/Utilities/Scripts/build-cube/find-cube.sh && \
     . /workspace/SDK/Utilities/Scripts/build-cube/una-version.sh \
       /workspace/Software/App/UnaWatch-CubeIDE/ && \
     /workspace/SDK/Utilities/Scripts/build-cube/build-cube.sh \
       /workspace/Software/App/UnaWatch-CubeIDE/'
```

The files above are written to `Exe/`. The version comes from the release tag that is checked out. After a commit of your own, it is that tag followed by your commit's hash, and `-dirty` if you have uncommitted changes. The build writes the version into `Software/App/UnaWatch-CubeIDE/.settings/org.eclipse.cdt.core.prefs` and rewrites `Software/App/UnaWatch-CubeIDE/.project`, and both count as changes; restore them with `git checkout -- Software/App/UnaWatch-CubeIDE/.settings Software/App/UnaWatch-CubeIDE/.project` before building again. To set the version yourself, add `-e BUILD_VERSION=<version>` to `docker run`.

A build of a release tag produces the same `.ota` as that release.

### Host unit tests

The kernel's unit tests run on your computer, without the watch. They need CMake and a C++17 compiler:

```bash
cmake -S Tests/Host -B build-host -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

## Installing on the watch

1. Charge the watch. The bootloader won't install an update when the battery is low.
2. Turn the watch off. Then hold the power button for 4 seconds as it turns on, until the bootloader menu appears.
3. Connect the watch to your computer with its USB cable. The watch shows "USB connected", and a drive appears on the computer.
4. Copy the `.ota` file to the root of that drive. To change the image the watch restores on a factory reset, also replace the `.gld` file already on the drive with the new one; the bootloader uses the first `.gld` it finds, so keep only one.
5. Eject the drive, then unplug the cable. The bootloader menu appears again.
6. Choose **Continue**. The watch installs the update, showing its progress, then starts the new kernel.

Every update has to be installed this way or through the UNA app; the bootloader accepts any version, including an older one.

## Repository layout

| Path | What it holds |
| --- | --- |
| `Software/App/UnaWatch-CubeIDE/` | The STM32CubeIDE project that builds the kernel |
| `Software/App/TouchGFX-GUI/` | The kernel's own screens (TouchGFX) |
| `Software/Libs/` | The kernel's source |
| `Software/ThirdParty/` | Third-party code, including `VendorLib/`: vendor code UNA compiles into a library because its source is not public |
| `Software/Utilities/Scripts/firmware_packer/` | Packs the build into `.ota`, `.gld` and `.hex` files |
| `SDK/` | The UNA SDK (submodule), which the kernel and apps share |
| `una-libs/` | The shared C library that the kernel and apps use, as source and as the prebuilt image the firmware ships with |
| `una-bootloader/` | The bootloader image and the update key; the bootloader's source is not included |
| `Tests/Host/` | Unit tests that run on a computer |

## Licence

UNA Watch Ltd's own code is under the MIT licence in [`LICENSE`](LICENSE). Third-party code, including the vendor library, is under its own terms: see [`THIRD-PARTY-LICENSES.md`](THIRD-PARTY-LICENSES.md).
