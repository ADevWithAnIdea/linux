.. SPDX-License-Identifier: GPL-2.0-only

Neo retained display sleep/wake
==============================

The T8140/J700 internal panel uses a retained DCP session with two AP host
links. Powering its CPU back on is insufficient: both host links and the AFK
services must complete their handshakes before IOMFB resumes display work.
The implemented protocol is for the qualified macOS 26.6.2 DCP firmware.

Handoff dependencies
--------------------

Use the matching J700 device tree, including the protected-host mailbox,
SID-24 DMA window, and display power-sequencer resources. The CPU and PMP
report domains must be allowed to idle; the primary CPU domain's AUTO policy
is restored by its sequencing client after the generic power transition.
Global clock/power-domain ignore-unused arguments are unnecessary.

The loader exports the current board's five opaque ``power-lut-data-*``
properties below the DCP's ``apple,provider-properties`` child. The kernel
serves those exact properties to firmware. Captured calibration tables must
not be substituted for the current board's data.

For the installed Fedora-style update-m1n1 hook, keep the stage-2 DTB selection
specific to this board in ``/etc/sysconfig/update-m1n1``::

    DTBS="/boot/dtb/apple/t8140-j700.dtb"

The versioned ``/boot/dtb`` link then follows kernel updates. The default
all-board expansion makes an unnecessarily large stage-2 copy on this device.
A live C-loader check rejected the complete all-board image plus SEP and
bootargs because its target crossed the protected image tail. Selecting J700
retains the same loader, board DTB and U-Boot while avoiding that overlap.
Validate the entire chainload allocation and physical target interval whenever
image composition or firmware changes; raw file size alone is insufficient.

Protocol order
--------------

Before retained CPU stop, IOMFB retires and acknowledges the clear frame.
All started AFK transports acknowledge their idle controls. The main AP link
then acknowledges idle, followed by the protected AP link, and finally the
shared IOP. Its idle notification permits mailbox IRQ/workqueue draining,
receive-FIFO disable, and CPU stop. The CPU-domain sideband controls follow
the matched PMGR transition order.

Resume prepares the existing RTKit session before restarting the CPU. Its
HELLO/EPMAP exchange preserves endpoint buffers and does not reissue application
endpoint starts. The protected host completes its own handshake, AFK services
resume, and IOMFB restores normal display operation.

Protected display requests use IRQ-driven 1024-byte DMA queues. Published
addresses are in 16-byte units. The observed state and restart methods receive
an empty reply and a final firmware acknowledgment. Unsupported methods or
arguments are rejected instead of being guessed. No additional DART implementation change or forced global TLB invalidation
is required by this retained path.

T8140 DCP also opts into OSLog type-2 cursor acknowledgments. Leaving a cursor
notification pending blocks the next AP-idle acknowledgment, so a first wake
can appear successful while the following sleep fails. This opt-in RTKit
behavior leaves other clients' OSLog handling unchanged.

Validation and limits
---------------------

Qualification checks both DCP runtime domains suspended while off, primary
DCP active after wake, normal panel timing, restored brightness, and completion
of a fresh compositor frame. Repeated cycles, a two-minute off interval, the
unchanged five-minute greeter idle policy, and wake via an actual input event
were exercised. The complete shared-core kernel also passed installed
NVMe/stage-2/U-Boot/GRUB boots; a module-only diagnostic is insufficient.

These checks cover display power management, not full system suspend. Protected
queue pointers remain owned by firmware, so the implementation pins their
module and buffers until reboot. Live unload and a complete firmware reset
are not supported by this retained path. An independent intermittent cold
DCP startup failure observed before sleep testing remains unresolved.
