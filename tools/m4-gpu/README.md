# M4 GPU driver bring-up

The `m4-gpu` branch starts from Asahi Linux 6.19.14,
`ea1ebe6e0347d315ad62efdae3f53272f7c4bbd5`. New code is authored by the
Gravity Linux Contributors and licensed GPL-2.0-only. The Rust module uses
the kernel's `GPL v2` MODULE_LICENSE spelling.

The initial port used the Python DRM shim as its behavioral reference. Ongoing
development is centered on the Rust driver; matching the shim's source
structure is no longer a requirement. The existing Asahi UAPI and frozen
restrictions still apply.

`CONFIG_DRM_ASAHI_M4` builds the Rust T8132 driver and binds `apple,agx-t8132`.
It starts the ASC firmware, constructs initialization data from source and
live bootloader inputs, and exposes `/dev/dri/card0` and `renderD128` with the
existing Asahi UAPI. Normal driver initialization is the default;
`asahi_m4.probe_only=1` selects the earlier identification-only mode.

The frontend uses DRM GEM shmem, scheduler entities, native syncobjs/timelines
and sync_file fences. SUBMIT copies commands and retains VM/buffer references,
then returns a pending completion fence. Input fences gate the execution worker.
One hardware job executes at a time, matching the known-working Python shim.
The worker polls firmware retirement with a bounded timeout; mapping changes
and context teardown drain the actual worker fences before detaching roots.
VM_DESTROY removes the public handle while existing queues retain the VM.
The last retaining queue releases its page-table trees and pinned backing.
This is asynchronous to userspace, with serialized hardware execution.

Render and compute have separate UAT views of a VM's shared GEM backing. The
port follows the shim's Work/microsequence encoding, fixed private namespaces,
resource-table snapshotting and per-context TVB ownership. TVB growth uses
source-built lists and retained pages. Firmware timestamp microcommands write
private per-Work slots at 24 MHz; the worker converts them to nanoseconds and
copies them into bound timestamp BOs before signaling completion. GET_TIME
uses the architectural counter and its own frequency.

GEM mappings and retired backing stay pinned until VM destruction. Firmware
Work storage and notifier links remain device-owned, as in the shim. A fatal
firmware failure fails subsequent work closed and retains potentially live
storage; recovery requires a device reboot. The frozen host document
`docs/m4-uapi-deviations.md` defines the port's accepted restrictions, including
fixed USC base, helper tuple rejection and local-indirect compute rejection.
No captured runtime initialization pages are loaded. The proprietary GPU
firmware itself is supplied by the platform boot chain.

The minimal J773g device tree contains the boot P-core, AIC3, UART, and GPU
power domain. Its register/interrupt definitions were adapted from Niklas
Sheth's source `linux-m4-integration` device trees. Storage, networking,
display, secondary Linux CPUs, and power-saving idle are outside this GPU
port. The kernel uses 16 KiB pages and boots at EL1 under m1n1.

## Build on the lab server

Source: `/home/lab/linux-m4`; Mac NFS mount:
`/Users/user/asahi_re/linux-m4`. Edit using ordinary local tools on the Mac.

```sh
ssh lab@192.168.170.255 \
  'bash /home/lab/linux-m4/tools/m4-gpu/build.sh'
```

The script uses LLVM 21.1.8, Rust 1.94.0 with `rust-src`, bindgen 0.72.1,
and `/opt/llvm-21/lib/libclang`. Output is `/home/lab/linux-m4-build`.
`M4_KERNEL_BUILD`, `M4_BUILD_JOBS`, and `M4_BUSYBOX` override the defaults.

The initramfs is constructed from the checked-in `init` shell script and
the lab's static AArch64 BusyBox, built from `/home/lab/busybox` at
`ec0c5cc` (clean source tree when verified). Kernel `gen_init_cpio` creates
the archive with explicit root ownership and device nodes, without sudo.
`SHA256SUMS` records the kernel, DTB, initramfs and BusyBox hashes.
If `/home/lab/linux-m4-build/mesa-runtime.tar.gz` exists, the build packages
that existing AArch64 Mesa runtime. `M4_MESA_RUNTIME` selects another archive;
the feature audit uses `/home/lab/linux-m4-build/mesa-feature-runtime.tar.gz`.
The build also compiles `async-smoke.c`, `feature-observe.c` and `uapi-options.c`.
The host `tools/m4_kernel_mesa_pack.py` creates the runtime archive without
modifying Mesa sources.

## Boot from the Mac

Use `/Users/user/asahi_re/m1n1-m4macmini`. The m1n1 tree includes the CTRR
reservation fix needed when iBoot's protected range lies below guest RAM.
Build m1n1 with `make -j8` after changing its C sources.

Copy these build artifacts into a new local artifact directory:

- `arch/arm64/boot/Image.gz`
- `arch/arm64/boot/dts/apple/t8132-j773g.dtb`
- `initramfs.cpio.gz`
- `System.map`
- `SHA256SUMS` and `build.log`

The examples use `build/m4-kernel/artifacts` and a **new** run directory
`build/m4-kernel/run`. The reset helper refuses an occupied serial port.
It resets **RID 2 only**, chainloads a fresh matching `build/m1n1.bin` using
the installed stage-1 ABI, then verifies the new proxy.

```sh
build/hv-venv/bin/python tests/hardware/reset_g16g.py \
  build/m4-kernel/run -c \
  'from m1n1.proxy import UartInterface; UartInterface().nop(); print("PROXY_ALIVE")'

build/hv-venv/bin/python tools/m4_kernel_payload.py \
  build/m4-kernel/artifacts build/m4-kernel/run/m1n1-linux.bin

env M1N1DEVICE=/dev/cu.usbmodemVY2TG2G6H11 PYTHONPATH=proxyclient \
  PYTHONUNBUFFERED=1 /usr/bin/script -q build/m4-kernel/run/hv.log \
  build/hv-venv/bin/python proxyclient/tools/run_guest.py \
  --proxy-console -C 6 -r -m tools/m4_kernel_hv.py \
  -c "load_system_map('build/m4-kernel/artifacts/System.map')" \
  build/m4-kernel/run/m1n1-linux.bin
```

`-C 6` exposes only the physical boot P-core (MPIDR `0x10100`) to the guest
m1n1. Keep this in sync with the minimal DT; it also prevents the guest
bootloader from starting CPUs omitted from Linux's memory reservations.
`nohlt` is this kernel's supported polling-idle argument (`idle=poll` is
not recognized on arm64 in this tree).

After `M4_USERSPACE_READY`, Ctrl-C enters the hypervisor Python shell.
These commands inject real UART input into the Linux shell and resume it:

```python
linux("uname -a; cat /proc/device-tree/model; echo")
# Ctrl-C again after output, then:
linux("readlink /sys/bus/platform/devices/302600000.gpu/driver; dmesg | grep asahi_m4")
```

The expected binding is `../../../../bus/platform/drivers/asahi_m4`.
Observed M4 GPU registers: ID `0x0a021100`, counts `0x0011010a` /
`0x00040404`, clusters `0x10030302`. Ctrl-C pauses the guest with the proxy
still available; it is not a Linux Ctrl-C. `cont` resumes it. The helper
only suppresses repeated successful UPMCR0 register-read messages; it does
not change register emulation.

Do not use RID 1 or the M5 serial port. Do not run another proxy client while
the hypervisor owns the M4 port.

## Focused Mesa checks

Run these from the booted Linux shell; `/opt/mesa/run` loads the existing Mesa
runtime and clears DRM-shim preloading. No Mesa source changes are required.

```sh
mount -t debugfs debugfs /sys/kernel/debug
/opt/mesa/run /bin/sh -c 'LD_PRELOAD=/async-smoke.so /opt/mesa/bin/g16-render 1 8 1 64 64 1'
/opt/mesa/run /bin/sh -c 'LD_PRELOAD=/async-smoke.so /opt/mesa/bin/g16-scratch 16 32 1 compute'
G16G_MIXED=1 /opt/mesa/run /opt/mesa/bin/g16-render 3 40000 2 64 64 4
/opt/mesa/run /opt/mesa/bin/g16-render 1 200000 2 64 64 4
```

`async-smoke.so` only wraps the Asahi UAPI: it supplies an externally delayed
input fence and timestamp BO destinations around actual Mesa commands.
`M4_UAPI_ASYNC_PASS` requires ioctl return before that fence is signaled and
Mesa output fences still pending and no early timestamp writes. `M4_UAPI_TIMESTAMP_PASS` checks nanosecond range,
ordering and guards. The Mesa callers independently check exact pixels or
compute output. The larger render has exercised firmware TVB growth from 11
to 51 blocks. Growth alone is not evidence of a partial render. The later feature audit
limited the TVB to 21 blocks and verified exact additive accumulation of all
200,000 triangles on two R32F targets across repeated growth refusals.

The host checks `tools/check_m4_kernel_wire.py`, `check_m4_kernel_render.py`
and `check_m4_kernel_cdm.py` compare a small set of actual Rust constructors
and CDM admission cases against Python. They are porting checks, not an
extensive hardware or conformance suite. Vulkan, display and multi-CPU Linux
qualification are outside the recorded validation.

Full build/boot artifacts remain in `m1n1-m4macmini/build/m4-kernel/`.
Selected logs, hashes and final qualification are in
`m1n1-m4macmini/docs/evidence/m4-kernel-20260913/` and
`m1n1-m4macmini/M4_EXPERIMENT_LOG.md`.

Final qualification: boot 020 / artifacts 019, kernel build 040 / release #30.
The exact feature ledger is in the host `docs/m4-kernel-uapi-coverage.md`;
evidence is under `docs/evidence/m4-kernel-20260913/uapi-completion/`.
The default TVB limit is 331 blocks. `asahi_m4.tvb_max_blocks=21` is only a
focused overflow diagnostic. `asahi_m4.fw_trace=1` requests firmware tracing,
but this firmware did not publish KTrace records; the partial-render oracle
therefore relies on exact accumulated output across observed refusals.

## Additional focused feature probes

The feature runtime includes the portable `suite-gl`, `suite-uapi` and
`libsuite-observe.so`, plus `gl-features`. Build the latter from this directory's
`gl-features.c` with `-I/path/to/drm-shim-suite/src -lEGL -lGLESv2 -ldl -lm`.
It reuses the portable suite's MIT-licensed GLES setup. Compile these callers
in the existing AArch64 Mesa environment and include their binaries in the
host runtime packer. They contain source shaders and use public UAPI calls.

Run only the targeted checks relevant to a change. Examples:

```sh
/opt/mesa/run /uapi-options
/opt/mesa/run /bin/sh -c 'LD_PRELOAD=/opt/mesa/bin/libsuite-observe.so SHIM_SUITE_BURST_SUBMITS=2 /opt/mesa/bin/suite-gl compute async 33 32 2 7'
/opt/mesa/run /bin/sh -c 'LD_PRELOAD=/feature-observe.so M4_UAPI_RSRC_SPEC=1 /opt/mesa/bin/gl-features partial'
/opt/mesa/run /bin/sh -c 'LD_PRELOAD=/feature-observe.so ASAHI_MESA_DEBUG= /opt/mesa/bin/gl-features compressed'
/opt/mesa/run /bin/sh -c 'LD_PRELOAD=/feature-observe.so M4_UAPI_LAYER_CLEAR=1 /opt/mesa/bin/suite-gl render solid 17 9 1 7'
/opt/mesa/run /bin/sh -c 'LD_PRELOAD=/feature-observe.so M4_UAPI_LAYER_CLEAR=1 M4_UAPI_NO_EMPTY=1 /opt/mesa/bin/suite-gl render solid 17 9 1 7'
```

The layer-clear probe replaces the draw with a source-built empty VDM stream
and checks both depth/stencil slices plus 48 KiB of guards. Its flag-clear arm
requires untouched ZLS memory. These are direct driver tests around Mesa tile
programs; the GL color oracle is intentionally bypassed for this probe.
`M4_TS_MASK=5/10` selects render start/end-only timestamps; `1/2` does the same
for compute. Mount debugfs before using `async-smoke.so`.

`feature-observe.so` can also set/reset all attachment stages, extend command
records, exercise VM-handle destruction and install four valid unused sampler
records. Graphics texture sampling passes with
`M4_SKIP_COMPUTE_SAMPLING=1 gl-features sampling`. This Mesa snapshot's compute
texture compiler is unimplemented; its layered framebuffer extension is also
not advertised. The experimental `layered-uapi` draw override completed with
wrong pixels and is not a passing layer oracle. Use the proven layer-clear
probe above. None of these Mesa limitations changes the frozen kernel UAPI
restrictions or the driver's source-built firmware state.

## Cleanup regression check (2026-09-13)

The completed-driver baseline is commit `778088823509`. The subsequent cleanup
shares mapping-coverage validation, names the accepted render flags, removes
unused timestamp arguments and unused utility/page-table APIs, and simplifies
render encoders while preserving narrowing conversions and ordered writes.
Core-dump-only masks now use the same configuration guard as their consumers.

Build 042 / kernel #32 completed without compiler warnings and booted as
boot 022 / artifacts 021. Fifteen existing Rust encoder outputs matched the
pre-cleanup Rust outputs byte for byte, including both Work/microsequence
cases and the 56,442,880-byte private-memory image. Hardware checks passed:
1,250 UAPI assertions, two pending compute jobs in both rounds, pending
queue/VM recreation, R/C/R/C mixed arrays, 200,000-triangle partial accumulation
with a 21-block TVB cap, compressed depth/stencil, both two-layer empty-tile
flag settings, and render/compute delayed dependencies and timestamps.
The final proxy NOP responded. Evidence is in the host repository under
`docs/evidence/m4-kernel-20260913/rust-cleanup/`; this was focused regression
coverage, not a full stress or conformance run.
