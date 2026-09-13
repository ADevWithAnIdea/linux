#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright The Gravity Linux Contributors
# Run on the lab build server. Source edits can be made through the NFS mount.
set -euo pipefail
source_dir=$(cd "$(dirname "$0")/../.." && pwd)
build_dir=${M4_KERNEL_BUILD:-/home/lab/linux-m4-build}
busybox=${M4_BUSYBOX:-/home/lab/busybox/busybox}
export PATH=/opt/llvm-21/bin:/home/lab/.cargo/bin:$PATH
export LIBCLANG_PATH=/opt/llvm-21/lib
export RUSTUP_TOOLCHAIN=1.94.0
make_args=(-C "$source_dir" O="$build_dir" ARCH=arm64 LLVM=1)
make "${make_args[@]}" rustavailable
make "${make_args[@]}" m4_gpu_defconfig
make "${make_args[@]}" -j"${M4_BUILD_JOBS:-48}" Image.gz apple/t8132-j773g.dtb

make "${make_args[@]}" -j8 headers_install INSTALL_HDR_PATH="$build_dir/uapi"
aarch64-linux-gnu-gcc -shared -fPIC -O2 -Wall -Wextra -Werror \
    -I"$build_dir/uapi/include" "$source_dir/tools/m4-gpu/async-smoke.c" \
    -o "$build_dir/async-smoke.so" -ldl -pthread

aarch64-linux-gnu-gcc -O2 -Wall -Wextra -Werror -I"$build_dir/uapi/include" \
    "$source_dir/tools/m4-gpu/uapi-options.c" -o "$build_dir/uapi-options"
aarch64-linux-gnu-gcc -O2 -Wall -Wextra -Werror -shared -fPIC \
    -I"$build_dir/uapi/include" "$source_dir/tools/m4-gpu/feature-observe.c" \
    -o "$build_dir/feature-observe.so" -ldl -pthread
aarch64-linux-gnu-gcc -O2 -Wall -Wextra -Werror -shared -fPIC \
    -I"$build_dir/uapi/include" "$source_dir/tools/m4-gpu/local-indirect.c" \
    -o "$build_dir/local-indirect.so" -ldl -pthread

# Use the lab's static AArch64 BusyBox built from /home/lab/busybox source.
# gen_init_cpio records root ownership and /dev/console without requiring root.
{
    for dir in bin sbin usr usr/bin usr/sbin dev proc sys run root tmp; do
        printf 'dir /%s 0755 0 0\n' "$dir"
    done
    printf 'nod /dev/console 0600 0 0 c 5 1\n'
    printf 'nod /dev/null 0666 0 0 c 1 3\n'
    printf 'file /bin/busybox %s 0755 0 0\n' "$busybox"
    printf 'file /async-smoke.so %s/async-smoke.so 0755 0 0\n' "$build_dir"
    printf 'file /uapi-options %s/uapi-options 0755 0 0\n' "$build_dir"
    printf 'file /feature-observe.so %s/feature-observe.so 0755 0 0\n' "$build_dir"
    printf 'file /local-indirect.so %s/local-indirect.so 0755 0 0\n' "$build_dir"
    printf 'file /init %s/tools/m4-gpu/init 0755 0 0\n' "$source_dir"
    for applet in sh mount umount mkdir hostname uname setsid cttyhack cat dmesg \
                  ls readlink hexdump sleep echo ps head tail wc grep sed awk \
                  free uptime stat sync reboot poweroff stty; do
        printf 'slink /bin/%s busybox 0777 0 0\n' "$applet"
    done
} > "$build_dir/initramfs.list"
mesa_runtime=${M4_MESA_RUNTIME:-$build_dir/mesa-runtime.tar.gz}
if [ -f "$mesa_runtime" ]; then
    python3 "$source_dir/tools/m4-gpu/mesa-initramfs.py" "$mesa_runtime" \
        "$build_dir/mesa-runtime" >> "$build_dir/initramfs.list"
fi
"$build_dir/usr/gen_init_cpio" -t 0 "$build_dir/initramfs.list" |
    gzip -n > "$build_dir/initramfs.cpio.gz"
sha256sum "$build_dir/arch/arm64/boot/Image.gz" \
    "$build_dir/arch/arm64/boot/dts/apple/t8132-j773g.dtb" \
    "$build_dir/initramfs.cpio.gz" "$busybox" > "$build_dir/SHA256SUMS"
cat "$build_dir/SHA256SUMS"
