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
and sync_file fences. SUBMIT copies commands, retains VM/buffer references and
returns a pending completion fence. Each public queue owns separate firmware
vertex, fragment and compute queues: descriptors, private state, ring pointers,
rings, counters, a notifier list and a firmware context. Priority selects the
shared firmware transport channel for each engine. Active queues lease distinct
event slots; idle queues release them after retirement. Pending submissions
retain their queue state after public handle destruction. Firmware allocations,
including retired queue storage, remain device-owned until reboot.
Each engine runs its active Work to completion. Preempting profiles lost compute
updates and render output on the M4, so priority chooses a firmware channel but
does not interrupt already executing work. Independent render and compute
engines overlap, and later tiling stages run while earlier fragments execute.

One publication/retirement worker admits work according to the selected engine's
channel and workqueue space, available ASIDs, event slots and TVB scene leases.
There is no fixed 16-command device window. A saturated queue waits while other
ready queues continue. Each VM has 36 scene slots, released after both stages
and event replies retire. Every render owns its tilemap, TPC, metadata, deflake,
status and auxiliary storage; concurrent renders do not overwrite that scratch.
Private-memory pools also have distinct hardware FList slots: reusing slot zero
for both engines caused render/compute execution stalls.

Unique completion stamps and advancing ring cursors determine retirement.
RTKit notifications wake the worker; ready scheduler jobs and firmware faults
also wake it. A generation counter closes the check/sleep race. The only runtime
timer is the ten-second retirement-progress watchdog; notifications do not
extend it. One-time bootstrap handshakes still use bounded polling. Mapping
changes and context teardown drain completion fences before detaching roots.

Input syncobj fences remain scheduler dependencies on actual completion. The
worker resolves UAPI VDM/CDM barrier indices independently: NONE permits
independent work, zero refers to the preceding submission, and N selects the
Nth preceding command on that engine in the current batch. Declared dependencies
wait for firmware completion events before publication, which also protects CPU
reads of GPU-produced CDM/resource metadata. Output syncobjs use actual completion
fences with independent fence contexts, so a later compute completion cannot
incorrectly imply completion of an earlier render.

Render and compute have separate UAT views of a VM's shared GEM backing. Each
queued compute owns a fork of the page-table tree and a private resource-table
snapshot at the caller's original DVA, plus private scratch and marker pages;
live roots are never rebound. Those views
remain owned until VM destruction. A driver-owned full CDM cache barrier followed
by a link to the caller stream preserves dependent SSBO writes between back-to-back
Works. The narrower 0x60000168 barrier was insufficient. The link was exercised
with a caller CDM address above 2 TiB; caller BO bytes remain unchanged.

TVB growth uses source-built lists and retained pages. Replies include the
request's subpipe and halt counter, including requests for queued work. Firmware
timestamp microcommands write nanoseconds directly into bound timestamp BOs
through the validated firmware aperture. Completion fences signal after the
firmware retires those writes. GET_TIME uses the architectural counter and its own frequency.
VM_DESTROY removes the public handle while existing queues retain the VM;
its final queue releases the page-table trees and pinned backing.

GEM mappings and retired backing stay pinned until VM destruction. Firmware
Work storage and notifier links remain device-owned, as in the shim. A fatal
firmware failure fails subsequent work closed and retains potentially live
storage; recovery requires a device reboot. The frozen host document
`docs/m4-uapi-deviations.md` defines the port's accepted restrictions, including
fixed USC base and helper tuple rejection. The subsequent user-requested
local-indirect integration removes that original compute-mode restriction.
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


## Memory publication review (2026-09-13)

Bulk client-page initialization queues cache cleans without a per-page DSB.
Address-space publication completes both roots with one barrier. Page-table
walks now track edited tables: reads do not allocate tracking entries or cause
cleaning, and subsequent syncs skip unchanged tables. Firmware storage tracks
pending initialization per page and skips previously published allocations,
including retained completed Work, instead of repeatedly cleaning all storage.

Live host updates preserve their cache invalidation/cleaning protocol, with
one barrier after all invalidations and one after all writes in a range.
Queue-private clearing uses one range (74 barriers become 2); a full ten-block
TVB list update also uses one range (20 become 2). The barriers ordering list
entries, counts, replies, doorbells and TLB invalidation remain. Live access
before a fresh page's initial sync preserves and publishes its CPU initialization.
No memory-lifetime policy or UAPI restriction changed.

Run the focused host check with `python3 tools/m4-gpu/check-memory.py`. It
compiles the actual Rust memory methods with page/cache stand-ins to check
dirty tracking, borrowed tables, metadata allocation failures, cross-page
writes, guard preservation and barrier counts. It is a bookkeeping model;
real hardware remains the test of cache/TLB ordering. Build 044 / #33 completed
without warnings. Boot 023 passed 1,250 UAPI assertions, pending VM teardown,
two concurrent compute jobs, mixed arrays, TVB growth/refusal and exact partial
accumulation, indirect compute, GPU page aliases, compressed depth/stencil,
compute spilling, external dependencies, and render/compute timestamps.
The final proxy NOP responded. Evidence is in the host repository's
`docs/evidence/m4-kernel-20260913/memory-publication/`.

## Local-indirect compute (2026-09-13)

The Rust CDM walker now admits mode 2's 24-byte launch and validates readable
coverage of its full six-word geometry object. Global-indirect still validates
three words. Geometry can be GPU-produced; the driver checks mappings rather
than reading dimensions on the CPU. The existing compute Work and asynchronous
execution path need no new firmware fields or helper binaries.

Build 045 / kernel #34, boot 024 / artifacts 023 passed the host's GLSL
producer/consumer workload: 18 local-indirect dispatches, 1D/2D/3D workgroups,
zero-work dimensions and 590,257 exact output/argument/guard checks. The same
workload passed again with a delayed native input fence and timestamps.
Global-indirect passed unchanged; deliberately selecting zero-work geometry
correctly failed the output oracle and subsequent work continued. UAPI checks
(1,250), R/C/R/C arrays, and exact 200,000-triangle partial accumulation through
TVB growth/refusal also passed. The final proxy NOP responded.

`local-indirect.c` is a test-only userspace ioctl interposer, built into the
initramfs as `/local-indirect.so`. It tracks the caller's VM bindings to map
the current Mesa CDM, changes global-indirect to local-indirect and selects
the producer's six-word object. It preserves shader fields and stream addresses.
It neither emulates DRM nor changes the kernel or geometry. Its linear-stream
and single-CDM-binding requirements are limits of this focused test adapter.
The source GLSL caller is `tests/hardware/g16g_local_indirect.c` in the host
m1n1 tree; include its compiled `local-indirect-gl` binary in the Mesa runtime
archive when building the initramfs.

```sh
/opt/mesa/run /bin/sh -c 'LD_PRELOAD=/local-indirect.so M4_LOCAL_INDIRECT=local /opt/mesa/bin/local-indirect-gl'
/opt/mesa/run /bin/sh -c 'LD_PRELOAD=/local-indirect.so M4_LOCAL_INDIRECT=global /opt/mesa/bin/local-indirect-gl'
# Negative control: output assertion must fail with exit 1.
/opt/mesa/run /bin/sh -c 'LD_PRELOAD=/local-indirect.so M4_LOCAL_INDIRECT=zero /opt/mesa/bin/local-indirect-gl'
mount -t debugfs debugfs /sys/kernel/debug
/opt/mesa/run /bin/sh -c 'LD_PRELOAD=/local-indirect.so:/async-smoke.so /opt/mesa/bin/local-indirect-gl'
```

The host `tools/check_m4_kernel_cdm.py` passes ten graph/admission cases plus
five geometry coverage/permission checks against the actual Rust parser.
Evidence and source/binary identities are in the host repository's
`docs/evidence/m4-kernel-20260913/local-indirect/`. This is focused driver
qualification, not geometry/tessellation shader conformance.

## Hardware pipeline checks

The host `tests/hardware/g16g_async_pipeline.c` uses GLES and the public Asahi UAPI.
It replays one Mesa command in separate SUBMITs, orders them with queue-header
barriers, and deliberately omits input-fence links between replays. Per-submission
syncobjs and timestamp slots verify completion and order; exact SSBO arithmetic
or additive pixels verify that earlier writes are visible. The test contains no
firmware layout or shader binary and reuses `drm-shim-suite/src/gl.c`.

Build it in the AArch64 Mesa environment with `-rdynamic`, the installed kernel
UAPI headers, and `-lEGL -lGLESv2 -ldl -lm`. Include the resulting `async-pipeline-gl`
in the host Mesa runtime packer. On a kernel booted with `asahi_m4.fw_trace=1`:

```sh
/opt/mesa/run /opt/mesa/bin/async-pipeline-gl 32 200000
/opt/mesa/run /bin/sh -c 'M4_PIPELINE_RENDER=1 /opt/mesa/bin/async-pipeline-gl 16 200000'
/opt/mesa/run /bin/sh -c 'M4_PIPELINE_HIGH_CDM=1 /opt/mesa/bin/async-pipeline-gl 8 200000'
```

For the partial-render check, boot with `asahi_m4.tvb_max_blocks=21`. The default
remains 331. `M4_PIPELINE_SERIAL=1` waits after every SUBMIT as a control;
`M4_PIPELINE_ATOMIC=1` selects atomic SSBO accesses. `M4_PIPELINE_DESTROY=1`
destroys the compute queue and VM before the normal completion wait and requires
that work was still pending when teardown began. The optional high-CDM check
requires a VM supporting 42-bit addresses. `unfinished_stamps=16` in the kernel
trace establishes actual firmware occupancy; pending userspace fences alone do
not establish that. `check-memory.py` also checks tree-fork ownership/failure
unwind and wrap-safe completion cursor comparisons using actual Rust methods.

Initial passing hardware evidence: build 053 / kernel #39, boot 030. Thirty-two
compute submissions pass through the 16-command window; sixteen partial renders
produce exactly 3,200,000 contributions, with growth/refusal on subpipes 0 and 1.
Mixed arrays, delayed inputs, timestamps, local-indirect geometry and UAPI checks
also pass. Build 054 / boot 031 additionally passes the above-2-TiB CDM alias.
Full evidence is in the host `docs/evidence/m4-kernel-20260913/hardware-pipeline/`.
This is focused regression coverage, not an extensive stress or conformance run.

Final driver qualification: warning-free build 057 / kernel #43, boot 032 / artifacts 032.
The pipeline, mixed, local-indirect, delayed-fence and timestamp checks pass again.
Boot 033 / build 058 has the identical kernel image and an updated test binary.
Destroying the queue/VM with 32 pending compute replays passes all 1,871 checks;
a fresh above-2-TiB compute pipeline then passes on the same device.

## Independent firmware queues

Build 066 / kernel #51 (boot 039) qualifies separate firmware queues on the M4.
The source caller `tests/hardware/g16g_async_pipeline.c` in the host repository
can distribute replays over eight public queues of both supported priorities.
Its multi-queue compute shader uses an order-independent atomic-add oracle;
timestamps enforce order only within each queue, and readback waits for all
queues. No Mesa source or shader binary fixture is changed.

```sh
/opt/mesa/run /bin/sh -c 'M4_PIPELINE_QUEUES=8 /opt/mesa/bin/async-pipeline-gl 32 200000'
/opt/mesa/run /bin/sh -c 'M4_PIPELINE_QUEUES=8 M4_PIPELINE_RENDER=1 /opt/mesa/bin/async-pipeline-gl 16 200000'
/opt/mesa/run /bin/sh -c 'M4_PIPELINE_QUEUES=8 M4_PIPELINE_DESTROY=1 /opt/mesa/bin/async-pipeline-gl 32 200000'
```

All three pass exact output and timestamp checks. The trace records distinct
firmware queues/events and a window of 16 unfinished Works. Four simultaneous
Mesa processes with disjoint buffers also pass, exercising event-slot reuse
after a queue goes idle. Mixed R/C/R/C, UAPI options, delayed input fences,
render/compute timestamps and GPU-produced local-indirect geometry pass on the
same boot. Evidence and failed controls are archived in the host repository at
`docs/evidence/m4-kernel-20260913/independent-queues/`.

Two formerly global fields needed queue-specific values: the tiling Work's
fragment event used for partial restart, and consecutive stamp sequences used
by the firmware dependency graph. Compute preemption remains disabled using
the same profile overrides as M1/M2; the preempting profile lost data even with
disjoint client buffers and private per-Work scratch. That qualification used
polling retirement. The global admission window and existing execution barriers
remain as described above. This is focused bring-up validation, not extensive
stress qualification.

## Firmware event wakeups

RTKit's threaded receive callback handles endpoint 0x20, message
0x0042000000000000 by advancing a condition-variable generation. The worker
samples that generation before inspecting jobs and firmware. Its wait checks
the same generation under the notification mutex, so a doorbell arriving
before sleep remains visible. Ready scheduler jobs and firmware faults use the
same wakeup path without acquiring the engine or firmware locks. Completion
events may coalesce: firmware stamps and ring consumers still determine when
timestamps, fences and context leases can retire.

The worker sleeps until a notification or its remaining retirement-progress
deadline. A watchdog expiry fails outstanding work and quarantines mappings;
it is never used to make successful completion progress. Unrelated firmware
traffic cannot restart the watchdog. The 16-command window and existing
execution ordering remain.

Removing the polling delay also exposed a general-barrier encoding omission.
Explicit dependencies now use internal_barrier_type=1 at G16 offset 0x30, as
M1/M2 does for queue dependencies. The fixed TA->fragment prelude retains zero.
The M4 firmware DAG checker skips dynamic stamp checks for a zero type; short
mixed batches without KTrace could consequently strand a compute->TA wait.

Public render and compute timestamps use the explicit user-timestamp
destinations in retained BO mappings. The initialization header advertises
their 64 MiB aperture at offset 0x28; the firmware rejects destinations outside
that interval. G16 Timestamp's user-pointer field is at microcommand offset
0x24. Firmware converts its 24 MHz ticks to nanoseconds before writing those
destinations, so the public frequency remains 1 GHz. Internal profiling storage
is separate and no CPU timestamp copy or conversion is needed.

`tools/m4-gpu/check-notify.py` executes the actual notification and general
barrier methods with host synchronization stand-ins. It checks notification
before sleep, 1,000 concurrent sleep/notify races, spurious wakeups, watchdog
expiry, sticky crash notification, generation wrap and the barrier encoding.
It does not emulate RTKit or GPU execution; those require hardware checks.

Build 074 / kernel #59, boot 045 / artifacts 045 qualifies this path with
KTrace disabled. Eight-queue compute (1,761 checks), eight-queue partial renders
(942), four concurrent Mesa clients (443 each), mixed R/C/R/C (1,210), public
UAPI options (1,250), and destruction with 32 submissions pending (1,893) pass.
Delayed input fences, exact rendering, GPU-produced local-indirect geometry
(590,257) and native render/compute timestamps also pass. The pipeline caller
now rejects zero timestamps, in addition to checking per-queue order.

The build is warning-free; notification, memory, wire and formatting checks
pass. The final proxy NOP responds and Linux is paused at the hypervisor prompt.
Raw passing and failed-control logs, artifact hashes and the source patch are
in the host repository's `docs/evidence/m4-kernel-20260913/firmware-events/`.
One-time bootstrap waits still use bounded polling. No Mesa source changes or
extensive stress/conformance testing were needed for this qualification.


## Render pipelining and queue backpressure

The host caller `tests/hardware/g16g_concurrency.c` captures source-built Mesa
work while retaining its resources, then submits it through the public UAPI.
It checks every rendered float and compute word, completion fences, timestamp
guards, stage order and overlapping stage intervals. `render` exercises tiling
of successive frames; `mixed` and `queues` interleave render/compute on one or
two public queues. `barriers` orders both engines, and `render-deps` orders only
framebuffer renders while compute remains independent. `pressure` submits a long
render backlog followed by compute and requires compute to bypass that backlog.
Compile/package it like `async-pipeline-gl` above, as `concurrency-gl`.

```sh
mount -t debugfs debugfs /sys/kernel/debug
/opt/mesa/run /opt/mesa/bin/concurrency-gl render 64 2000000
/opt/mesa/run /opt/mesa/bin/concurrency-gl mixed 32 200000
/opt/mesa/run /opt/mesa/bin/concurrency-gl queues 8 200000
/opt/mesa/run /opt/mesa/bin/concurrency-gl barriers 8 200000
/opt/mesa/run /opt/mesa/bin/concurrency-gl render-deps 8 200000
/opt/mesa/run /opt/mesa/bin/concurrency-gl pressure 64 2000000
/opt/mesa/run /opt/mesa/bin/concurrency-gl compute 128 2000000
```

Boot argument `asahi_m4.fw_trace=2` reports each saturated admission resource
once, without enabling KTrace or logging every publication. `fw_trace=1`
retains the previous KTrace/publication diagnostics. Normal operation uses zero.
The earlier 16-command window and shared-render-scratch restrictions described
in the historical qualification sections above are superseded by this change.
Engine preemption remains disabled; cross-engine overlap and successive tiling/
fragment execution do not require it. Firmware timestamps establish overlapping
stage intervals, not a measurement of simultaneous instruction issue on USCs.

Build 102 / kernel #80 / boot 070 passes all the calls above, sixteen partial
renders on eight queues, mixed command arrays, UAPI options, delayed fences
and timestamps, local indirect compute, and teardown with 32 commands pending.
Admission actually reaches 36 occupied scenes and 63 occupied ASIDs. The final
compute in the pressure case finishes 1.048 seconds before the last render
starts tiling. Host scheduling, memory, notification and wire checks also pass.
Evidence and hashes are in the host repository at
`docs/evidence/m4-kernel-20260913/concurrency/`.
