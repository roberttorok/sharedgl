# SharedGL: the `windows-support` branch

> **This branch is meant to be used with [qemu-xp-3d](https://github.com/roberttorok/qemu-xp-3d).** Windows hosts need a patched QEMU (the `ivshmem-win` device).

Extends [SharedGL](https://github.com/dmaivel/sharedgl) to **Windows XP guests** and **Windows hosts**, and makes the Windows client faster and more robust. Direct3D 8/9 games on XP run on the host GPU through [WineD3D](https://github.com/JHRobotics/wine9x).

Tested: FIFA 2004, GTA III, 3DMark 2001 SE (24,586 points); XP SP3 guest, RTX 3070, Linux and Windows 11 hosts.

Each change is a separate commit, in order of importance.

### Added

1. **Windows XP guests.**
   - Drop-in `opengl32.dll` for 32-bit XP. XP's own `opengl32.dll` can't load an ICD through the QXL display driver.
   - `kernel/winxp/sgldrv.sys`: WDM driver for ivshmem, with INF and installer (`ksgldrv` needs KMDF 1.15, which XP lacks). Maps the memory through `\Device\PhysicalMemory`, because an XP MDL can't describe 64 MiB or more.
   - Frames are blitted with GDI from a separate thread with a triple buffer, so games don't wait for GDI.
   - Frame rate cap set only by the host (`sglrenderer -f`, default 60). The game's swap interval is ignored.
   - `sgldrv.sys` maps the QXL display memory write-back instead of uncached: each frame's blit gets about 10× cheaper on AMD hosts (`sglrenderer -q` turns it off).
   - Connects in `wglCreateContext`, not at DLL load, because GDI loads `opengl32.dll` into processes that never draw.
   - 16-bit display mode requests become 32-bit, because QEMU shows 16-bit QXL modes wrongly.
2. **Windows hosts.** The server builds and runs on Windows. Shared memory is the named file mapping `Local\sharedgl_shared_memory`. QEMU for Windows has no ivshmem memory backend, so it needs the `ivshmem-win` device from qemu-xp-3d.
3. **Faster streaming buffer maps.** WineD3D maps vertex buffers hundreds of times per frame, each a round trip. Write-only maps now use a client-side copy and send only changed blocks; invalidated maps need no reply; large replies come in one piece.
4. **Pixel buffer objects and missing functions:** pack/unpack buffer offsets, `glReadPixels`, `glDrawPixels`, `glGetCompressedTexImage`, `glGetBufferSubData`, `glSamplerParameter{i,f,Ii,Iui}v`.
5. **Debugging and measurement aids** (off by default): see the tables below, plus command traces and logging of failed shader compiles and links.

### Fixed

1. **Several buffers mapped at once.** Only one mapping was tracked, so a second one (e.g. vertex and index buffer together) sent data to the wrong buffer. GTA III crashed two seconds into driving.
2. **Deleted buffer still bound to an enabled vertex attribute** crashed: its offset was read as a memory address.
3. **Heap corruption on flush and unmap** through a stale mapping pointer.
4. **`glCompressedTexSubImage1D/2D`** read arguments the client never sends.
5. **Uniform arrays**: every element went to the first element's location.
6. Smaller: frame readback no longer depends on the app's read framebuffer and pack state; `glBitmap` floats and size; 64-bit query results on Windows; `glGetError` looping without a current context on NVIDIA/Windows; the server handles `SIGTERM` and tells later clients it has stopped.

### Changed

1. The server spins for 1 ms while waiting for commands, then naps, instead of using a whole core.
2. The client spins briefly, then sleeps while waiting for the server (`SGL_WAIT_MODE=original`: the old loop).
3. Windows clients map shared memory write-back instead of write-combined (write-combined reads are uncached), find the device by interface class (works on XP), and fail cleanly with no server. **Affects Windows 10 guests too; untested there.**
4. `inc/lzav.h` uses LF line endings.

### New server options

| Option | Effect |
|---|---|
| `-f FPS` | Frame cap for XP clients, `0` = none. Default 60. Read by the client from shared memory every frame |
| `-q` | Leave XP's QXL display memory uncached |

### New environment variables

| Variable | Where | Effect |
|---|---|---|
| `SGL_SYNC_PRESENT=true` | XP guest | Blit from the game's thread instead of a separate one |
| `SGL_ALLOW_16BPP=true` | XP guest | Allow 16-bit display modes |
| `SGL_STATS` | XP guest | Log frame rates, unimplemented functions and crash addresses to `%TEMP%\sharedgl.log` |
| `SGL_CLIENT_PROFILE` | XP guest | Record client timings in shared memory |
| `SGL_WAIT_MODE=original` | Windows guests | Old wait loop |
| `SGL_NO_SHADOW_MAPS` | Guest | No client-side copies for streaming maps |
| `SGL_SMALL_DOWNLOADS` | Guest | No one-piece downloads |
| `SGL_DEFER_MAPS` | Guest | Experimental: send only flushed ranges of a map |
| `SGL_PROFILE` | Server | Time per command, waits and context switches, to `sgl_profile.txt` every 5 s |
| `SGL_FRAMEPROF` | Server | Per-frame guest and host timings to a CSV; see qemu-xp-3d's `PROFILING.md` |
| `SGL_FRAMETIME` | Server | Microseconds between swaps, to `sgl_frametime.log` |
| `SGL_SHADERDUMP` | Server | Shaders of each program, to `sgl_shaders.txt` |
| `SGL_DRAWCHECK` | Server | Print the last draw's vertex arrays on a crash |

The server also writes the last 5 s frame rate to `sgl_fps`, and traces N frames of commands to `sgl_trace.log` when N is written to `sgl_trace_frames`. These files are in `/dev/shm` on Linux, the working folder on Windows.

### Building the new parts

CMake builds the Linux server and client as before. The XP client and driver and the Windows server use MinGW GCC directly (qemu-xp-3d's Makefile does all of this):

```sh
# XP client and driver (Linux: i686-w64-mingw32-gcc; MSYS2: the MINGW32 gcc)
CC=i686-w64-mingw32-gcc
CFLAGS="-march=k8-sse3 -mtune=generic -mfpmath=sse -mstackrealign -O2 -s -w -D_WIN32_WINNT=0x0501 -DWINVER=0x0501 -Iinc -DSGL_XP -DSGL_NO_NETWORK"
for f in winmain pb spinlock scratch; do $CC $CFLAGS -c src/client/$f.c -o $f.o; done
$CC $CFLAGS -c src/client/platform/windrv.c -o windrv.o
# -mrtd makes the GL entry points stdcall; the -fno-builtin flags keep GCC from calling printf and friends as stdcall
$CC $CFLAGS -mrtd -fno-builtin-fprintf -fno-builtin-printf -fno-builtin-fputs -fno-builtin-fputc \
    -fno-builtin-puts -fno-builtin-putchar -c src/client/glimpl.c -o glimpl.o
$CC -shared -s -o opengl32.dll winmain.o pb.o spinlock.o glimpl.o scratch.o windrv.o \
    -Wl,--kill-at -Wl,--export-all-symbols -static-libgcc -lsetupapi -lgdi32 -luser32 -lwinmm
$CC -O2 -s -march=pentium2 -w -fno-stack-protector -fno-stack-check -fno-aggressive-loop-optimizations \
    -fno-asynchronous-unwind-tables -D_X86_=1 -I/usr/share/mingw-w64/include/ddk \
    -shared -nostdlib -nostartfiles -o sgldrv.sys kernel/winxp/sgldrv.c \
    -Wl,--subsystem,native:5.1 -Wl,--major-os-version,5,--minor-os-version,1 -Wl,--entry,_DriverEntry@8 \
    -Wl,--enable-reloc-section -Wl,--image-base,0x10000 -Wl,--disable-nxcompat -Wl,--disable-dynamicbase \
    -lntoskrnl -lhal
$CC $CFLAGS -o sglinstall.exe kernel/winxp/sglinstall.c -lnewdev -static-libgcc

# Windows server (MSYS2 MINGW64 shell)
gcc -O2 -std=gnu17 -w -DSDL_MAIN_HANDLED -Iinc src/server/context.c src/server/dynarr.c src/server/frameprof.c \
    src/server/main.c src/server/overlay.c src/server/processor.c src/server/sgldebug.c src/client/scratch.c \
    -o sglrenderer.exe -lSDL2 -lepoxy -lws2_32 -lwinmm -liphlpapi
```

- **XP install:** put `sgldrv.sys`, `sgldrv.inf` and `sglinstall.exe` in one folder, run `sglinstall.exe`, and copy `opengl32.dll` next to the game's executable.
- **MSYS2:** the driver's `-I` is `/mingw32/include/ddk`. Add `-std=gnu17` to every command (GCC defaults to C23, which this code doesn't compile as), and `-D__USE_MINGW_ANSI_STDIO=0` to `CFLAGS` so the client uses XP's own `printf`.

---

# Original README

The upstream README follows, unchanged.

---

<img align="right" width="8%" src="https://github.com/dmaivel/sharedgl/assets/38770072/7ad42df9-d10c-413a-9ef4-7682c06dc679">

# SharedGL ![license](https://img.shields.io/badge/license-MIT-blue)

SharedGL is an OpenGL 4.6 implementation that enables 3D acceleration for Windows and Linux guests within QEMU/KVM by streaming OpenGL commands over shared memory or sockets.

<details open>
<summary>Click to hide: Preview</summary>

![preview](https://github.com/user-attachments/assets/fbcd62ac-090d-4ad1-b37c-bd1f0432866a)

</details>

<details>
<summary>Click to reveal: Table of contents</summary>

1. [How it works](#how-it-works)
2. [Building the server](#building-the-server)
3. [Running the server](#running-the-server)
4. [Connecting the guest](#connecting-the-guest)
   - [Choosing a transport](#choosing-a-transport)
   - [Windows guest](#windows-guest)
   - [Linux guest](#linux-guest)
   - [Linux host (no VM)](#linux-host-no-vm)
   - [Environment variables](#environment-variables)
5. [Troubleshooting](#troubleshooting)
6. [Showcase](#showcase)

</details>

# How it works

SharedGL has two halves:

- Server (`sglrenderer`) which runs on the host, Linux only. Receives OpenGL commands and renders them in a window on the host.
- Client (ICD / `libGL`) which runs in the guest. Installed inside the VM so that OpenGL applications there transparently forward their calls to the server.

The two halves communicate through one of two transports:

| Transport | Speed | Setup |
|-----------|-------|-------|
| Shared memory  | Faster | Default, requires an `ivshmem` device on the VM and a kernel driver in the guest. |
| Network socket | Slower | No drivers, no VM config. It works anywhere with a network connection, so it's not restricted to just VMs. |

You pick the transport when you start the server. The rest of this README follows that order: build the server, run it, then connect a guest.

# Building the server

### Dependencies

| Name | Version |
|------|---------|
| [CMake](https://cmake.org/) | 3.15+ |
| [libepoxy](https://github.com/anholt/libepoxy) | Latest |
| [SDL2](https://www.libsdl.org/) | 2.24.0+ |

### Prebuilt tarball

If you just want to run the Linux server, you can download the Linux release tarball instead of building from source.

### Build

```bash
git clone https://github.com/dmaivel/sharedgl.git
cd sharedgl
mkdir build && cd build
cmake ..
cmake --build . --target sglrenderer --config Release
```

The server can only be built and run on a Linux host.

If you also want the Linux client library (`libGL`) for use on the host or to copy into a Linux guest, build the `sharedgl-core` target instead. `libx11` is required for this.

### Build options

Pass these to `cmake` with `-D...`, or edit them via `ccmake build`.

| Option | Values | Default | Description |
|--------|--------|---------|-------------|
| `LINUX_LIB32` | `ON`/`OFF` | `OFF` | Build the Linux client library as 32-bit. Does not affect the server. |

# Running the server

```
usage: sglrenderer [-h] [-v] [-o] [-n] [-x] [-g MAJOR.MINOR]
                   [-r WIDTHxHEIGHT] [-m SIZE] [-p PORT]
```

| Flag | Description |
|------|-------------|
| `-h` | Show help |
| `-v` | Print sample VM configuration for the current settings |
| `-o` | Enable FPS overlay on clients |
| `-n` | Use networking instead of shared memory |
| `-x` | Remove the shared memory file (useful for cleanup) |
| `-g MAJOR.MINOR` | Report a specific OpenGL version (default: `4.6`) |
| `-r WxH` | Max resolution (default: `1920x1080`) |
| `-m SIZE` | Max memory in MiB (default: `32`) |
| `-p PORT` | Port when `-n` is used (default: `3000`) |

The server must be running on the host before you start the guest. If you extracted a Linux release tarball, run `./sglrenderer` from the extracted root.

# Connecting the guest

## Choosing a transport

Before you install anything in the guest, decide how the guest will reach the server:

- Shared memory: default, faster. You'll need to add an `ivshmem` device to the VM and install a kernel driver in the guest.
- Networking: driverless, simpler, much slower. Start the server with `-n`, and set `SGL_NETWORK_ENDPOINT=<host-ip>:<port>` as an environment variable inside the guest.

### VM configuration for shared memory

Run `sglrenderer -v` to get configuration snippets matched to your current settings. For reference:

**libvirt:**
```xml
<!-- SAMPLE, place inside <devices> -->
<devices>
    ...
    <shmem name="sharedgl_shared_memory">
        <model type="ivshmem-plain"/>
        <size unit="M">??</size>
    </shmem>
</devices>
```

**qemu:**
```bash
# SAMPLE
qemu-system-x86_64 \
  -object memory-backend-file,size=??M,share,mem-path=/dev/shm/sharedgl_shared_memory,id=sharedgl_shared_memory
```

## Windows guest

A Windows guest needs two things: a kernel driver (for shared memory only) and the OpenGL client library (the ICD).

### 1. Kernel driver

Pick one of the three options below.

#### Option A: Stock VirtIO IVSHMEM driver (single client only)

1. Download and extract the [upstream VirtIO Win drivers](https://fedorapeople.org/groups/virt/virtio-win/direct-downloads/upstream-virtio/).
2. Go to `...\virtio-win-upstream\Win10\amd64\`.
3. Right-click `ivshmem.inf` and choose Install.

#### Option B: Stock VirtIO IVSHMEM driver + runtime patch (multi-client)

Same driver as Option A, but patched at runtime by [ntoseye](https://github.com/dmaivel/ntoseye) to allow multiple clients. The patch must be re-applied every time the VM boots.

1. Follow Option A to install the stock driver.
2. Install and run `ntoseye` (see its README if you hit issues).
3. In the `ntoseye` console, run:
   ```
   lm ivshmem                          # prints start address
   s <start> 4883792000740a 0x3000     # prints match address
   f <match> 4883792000eb0a            # applies the patch
   continue
   quit
   ```

#### Option C: Bundled `ksgldrv` driver (multi-client, Windows 10)

Requires enabling test-signing once:

```
bcdedit.exe -set testsigning on
```

Then either install from a release or build from source:

<details>
<summary>Install from release (release 0.4.0)</summary>

1. Download and extract the latest Windows release.
2. Right-click `ksgldrv.inf` and choose Install.

</details>

<details>
<summary>Build from source (Visual Studio Developer Command Prompt)</summary>

Install the [WDK](https://learn.microsoft.com/en-us/windows-hardware/drivers/other-wdk-downloads) first, then:

```bat
:: from the sharedgl directory
mkdir build
cd build
cmake -DCMAKE_GENERATOR_PLATFORM=x64 -DWINKERNEL=ON ..
cmake --build . --target ksgldrv --config Release
cd ..
xcopy .\scripts\kcertify.bat .\build\Release\kcertify.bat
xcopy .\scripts\ksgldrv.inf .\build\Release\ksgldrv.inf
cd build\Release
call kcertify.bat 10_X64

:: requires admin, or right-click ksgldrv.inf and choose Install
pnputil -i -a ksgldrv.inf
```

The default target is `10_X64`. For other or additional Windows versions, pass them to `kcertify.bat` (e.g. `kcertify.bat 10_X64,10_NI_X64`). See the [OS version list](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/inf2cat).

</details>

### 2. Client library (ICD)

Install from a release or build from source.

<details>
<summary>Install from release (>= 0.3.1)</summary>

1. Download and extract the latest Windows release.
2. Run `wininstall.bat` as administrator.

Any OpenGL application (32-bit or 64-bit) will now route through SharedGL.

</details>

<details>
<summary>Build from source (Visual Studio Developer Command Prompt)</summary>

```bat
:: from the sharedgl directory
mkdir build
cd build
:: add -DWINKERNEL=OFF if you hit WDK errors
cmake -DCMAKE_GENERATOR_PLATFORM=x64 ..
cmake --build . --target sharedgl-core --config Release
cmake -DCMAKE_GENERATOR_PLATFORM=Win32 ..
cmake --build . --target sharedgl-core --config Release
cd ..
xcopy .\scripts\wininstall.bat .\build\Release\wininstall.bat
cd build\Release
call wininstall.bat
```

</details>

## Linux guest

Although it's possible to use SharedGL with Linux guests, there is little reason to given first-class 3D acceleration support via virtio/virgl.

A Linux guest needs a kernel module (for shared memory only) and the client library.

### 1. Kernel module

Build inside the guest, then load with the provided installer:

```bash
cd kernel/linux
make
./install.sh
```

If the module doesn't auto-load on boot, add `sharedgl` to your modprobe config.

### 2. Client library

Build `sharedgl-core` as described in [Building the server](#building-the-server), or use the packaged client libraries from the Linux release tarball. Then point your loader at the client directory:

```bash
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/path/to/sharedgl/lib
glxgears
```

Use `lib32/` instead of `lib/` for 32-bit applications. Some applications also need `LD_PRELOAD` set to the same directory.

## Linux host (no VM)

The Linux client library also works directly on the host, useful for debugging without a VM. Build `sharedgl-core` or use the packaged tarball, set `LD_LIBRARY_PATH` as shown in the [Linux guest](#linux-guest) section, and run your application.

## Environment variables

Once the client is installed, these variables tune its behavior. `SGL_NETWORK_ENDPOINT` is required when the server is running in networking mode; the rest are optional. Variables marked `host` inherit from the server when not overridden.

Boolean env vars can only be enabled by setting them to `true`, not `1` or `on`.

| Variable | Values | Default | Platform | Description |
|----------|--------|---------|----------|-------------|
| `SGL_NETWORK_ENDPOINT` | `IP:Port` |  | Windows, Linux | Required in the guest when networking is enabled. |
| `SGL_WINED3D_DONT_VFLIP` | boolean | `false` | Windows | Set to `true` when running DirectX apps through WineD3D so the framebuffer renders right-side up. |
| `SGL_RUN_WITH_LOW_PRIORITY` | boolean | `false` | Windows | Runs the client at `IDLE_PRIORITY_CLASS`. Can improve smoothness on VMs with fewer vCPUs than host cores, or when using networking. |
| `GL_VERSION_OVERRIDE` | `D.D` | `host` | Windows, Linux | Override the reported OpenGL version. |
| `GLX_VERSION_OVERRIDE` | `D.D` | `1.4` | Linux | Override the reported GLX version. |
| `GLSL_VERSION_OVERRIDE` | `D.D` |  | Windows, Linux | Override the reported GLSL version. |

# Troubleshooting

#### Client logs `glimpl_init: failed to find memory`
The VM doesn't have a shared memory device attached. Add the `ivshmem` config from [VM configuration for shared memory](#vm-configuration-for-shared-memory), or switch to networking.

#### Server logs `err: failed to open shared memory 'sharedgl_shared_memory'
The shared memory file already exists with different permissions. Shut down the VM, run `sudo ./sglrenderer -x` to remove it, then start the server and VM again. Running the server with `sudo` is a shorter-term workaround.

#### Application stalls in the VM (process starts but hangs)
- Confirm the server is running on the host. If it is and the app still stalls, shut down the VM, run `sudo ./sglrenderer -x`, restart the server, then boot the VM.
- Confirm the driver is installed (VirtIO IVSHMEM or `ksgldrv` on Windows; `sharedgl.ko` on Linux).

#### Blank window in the VM
The shared memory device likely isn't sized to cover all the memory the server allocates. Check the `<size>` in your VM config against the server's `-m` value.

#### Crashes like `IOT instruction` or `No provider of glXXX found.`
- Try a lower reported GL version: `-g 2.0` or env vars
- Give the server more memory: `-m 256`

#### GLFW apps report "Entry point retrieval is broken"
Use `LD_PRELOAD` with the client library.

#### Linux guest crashes with `SIGILL`
The client library was built on a host with a different CPU than the guest sees.

# Showcase

<details>
<summary>Click to reveal: Running SuperTuxKart in a Windows virtual machine</summary>

https://github.com/dmaivel/sharedgl/assets/38770072/c302f546-2c05-4cb7-b415-8f01ad1dce7a

</details>

<details>
<summary>Click to reveal: Running a DirectX sample via WineD3D in a Windows virtual machine</summary>

https://github.com/dmaivel/sharedgl/assets/38770072/f2ae2825-79d6-4c1b-813f-c826586642e2

</details>

<details>
<summary>Click to reveal: Running minetest in a Windows virtual machine (old)</summary>

https://github.com/dmaivel/sharedgl/assets/38770072/26c6216d-72f7-45b4-9c4f-1734de0d1c6d

</details>

<details>
<summary>Click to reveal: Running glxgears in a Windows virtual machine (old)</summary>

https://github.com/dmaivel/sharedgl/assets/38770072/a774db97-807e-46b9-a453-fa2ee3f4ea84

</details>

<details>
<summary>Click to reveal: Running glxgears in a Linux virtual machine (old)</summary>

https://github.com/dmaivel/sharedgl/assets/38770072/0d46bf46-5693-4842-a81f-2f186c396e26

</details>

<details>
<summary>Click to reveal: Running a compute shader in a Linux virtual machine (old)</summary>

https://github.com/dmaivel/sharedgl/assets/38770072/ded179b8-23dc-491d-ba34-4108e014f296

</details>
