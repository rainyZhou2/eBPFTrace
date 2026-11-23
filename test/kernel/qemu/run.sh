#!/usr/bin/env bash
# Runs etrace_kernel_tests inside a throwaway QEMU VM.
#
# The kernel is a distribution package (Ubuntu 5.15 or 6.8), so the CI
# matrix can cover two kernel generations without building one. The guest
# runs entirely from an initramfs built from the test binary, the scenario
# binaries and a static busybox. The C library is copied from the Debian
# trixie base the builder image is built on, so the host userspace does not
# matter and the guest kernel only has to support the syscalls it uses.
#
# The test needs root, BTF and a host PID namespace. Inside the VM it is
# the guest's PID 1 parent, so the tests compare host thread ids correctly
# without privileged containers or host namespace access.
#
# usage:
#   test/kernel/qemu/run.sh --kernel-image ubuntu:22.04 --expect 5.15 \
#       [--test-bin bin/etrace_kernel_tests] [--scenarios test/scenario] \
#       [--filter '-Kernel.UserStackWithoutFramePointers:Kernel.GpuWaits'] \
#       [--smp 2] [--timeout 3600]
#
# Requires: docker, qemu-system-x86_64, cpio, gzip, timeout (coreutils), dpkg.
# Only x86_64 guests are supported; arm64 is covered by the unit test job.

set -euo pipefail

die() {
	printf 'run.sh: %s\n' "$*" >&2
	exit 1
}

kernel_image=ubuntu:22.04
expect=
test_bin=bin/etrace_kernel_tests
scenarios=test/scenario
filter=
smp=2
timeout_s=3600

while [ $# -gt 0 ]; do
	case "$1" in
	--kernel-image)
		kernel_image=${2:?}
		shift 2
		;;
	--expect)
		expect=${2:?}
		shift 2
		;;
	--test-bin)
		test_bin=${2:?}
		shift 2
		;;
	--scenarios)
		scenarios=${2:?}
		shift 2
		;;
	--filter)
		filter=${2-}
		shift 2
		;;
	--smp)
		smp=${2:?}
		shift 2
		;;
	--timeout)
		timeout_s=${2:?}
		shift 2
		;;
	-h | --help)
		sed -n '2,28p' "$0"
		exit 0
		;;
	*)
		die "unknown argument: $1"
		;;
	esac
done

[ "$(uname -m)" = x86_64 ] || die "only x86_64 guests are supported (got $(uname -m))"
[ -f "$test_bin" ] || die "test binary not found: $test_bin (run make kernel-bin)"
[ -d "$scenarios" ] || die "scenario directory not found: $scenarios (run make scenarios)"
# GitHub artifacts do not preserve the executable bit.
chmod +x "$test_bin"
for cmd in docker qemu-system-x86_64 cpio gzip timeout dpkg-deb; do
	command -v "$cmd" >/dev/null || die "missing required command: $cmd"
done

cleanup() {
	if [ -d "${work:-}" ]; then
		# Files written by the containers below are root-owned; remove them
		# through a container so a non-root runner can delete the directory.
		docker run --rm -v "$work:/w" debian:trixie sh -c 'find /w -mindepth 1 -delete' >/dev/null 2>&1 || true
		rm -rf "$work" 2>/dev/null || true
	fi
	return 0
}

work=$(mktemp -d)
trap cleanup EXIT
mkdir -p "$work/debs" "$work/kernel" "$work/root"

echo "==> downloading kernel from $kernel_image"
docker run --rm -v "$work/debs:/out" -e DEBIAN_FRONTEND=noninteractive "$kernel_image" bash -c '
	set -e
	apt-get update -qq
	apt-get install -y -qq --no-install-recommends --download-only \
		-o Dir::Cache::archives=/out linux-image-generic >/dev/null
' >/dev/null

deb=
for candidate in "$work"/debs/linux-image-[0-9]*.deb; do
	if [ -e "$candidate" ]; then
		deb=$candidate
		break
	fi
done
[ -n "$deb" ] || die "the kernel image package was not downloaded"
dpkg-deb -x "$deb" "$work/kernel"
vmlinuz=
for candidate in "$work"/kernel/boot/vmlinuz-*; do
	if [ -e "$candidate" ]; then
		vmlinuz=$candidate
		break
	fi
done
[ -n "$vmlinuz" ] || die "no vmlinuz in $deb"
if [ -n "$expect" ]; then
	case "$(basename "$vmlinuz")" in
	*"$expect"*) ;;
	*) die "kernel $(basename "$vmlinuz") does not match --expect $expect" ;;
	esac
fi
echo "==> kernel $(basename "$vmlinuz")"

echo "==> assembling guest rootfs"
cp "$test_bin" "$work/root/kt"
mkdir -p "$work/root/bin" "$work/root/scen" "$work/root/proc" "$work/root/sys" "$work/root/dev" "$work/root/tmp" "$work/root/etc"
cp -a "$scenarios/." "$work/root/scen/"
# The scenario binaries come from the same artifact and need their mode back.
chmod +x "$work/root/kt"
for bin in lockchain-amd64 nofp-amd64 traced-amd64 gpu-amd64; do
	if [ -f "$work/root/scen/$bin" ]; then
		chmod +x "$work/root/scen/$bin"
	fi
done
printf 'root:x:0:0:root:/root:/bin/sh\n' >"$work/root/etc/passwd"
printf '127.0.0.1 localhost\n' >"$work/root/etc/hosts"
if [ -n "$filter" ]; then
	printf '%s' "$filter" >"$work/root/etc/gtest_filter"
fi

# The binary is linked against the builder image's glibc; copy that exact
# loader and libraries instead of whatever the CI host has.
docker run --rm -v "$work/root:/rootfs" -v "$(cd "$(dirname "$test_bin")" && pwd)/$(basename "$test_bin"):/kt:ro" \
	-e DEBIAN_FRONTEND=noninteractive debian:trixie bash -c '
	set -e
	apt-get update -qq
	apt-get install -y -qq --no-install-recommends busybox-static >/dev/null
	install -m 0755 /bin/busybox /rootfs/bin/busybox
	for bin in /kt /rootfs/scen/*; do
		[ -f "$bin" ] || continue
		if LD_LIBRARY_PATH=/rootfs/scen ldd "$bin" 2>/dev/null | grep -q "not found"; then
			echo "unresolved shared library in $bin:" >&2
			LD_LIBRARY_PATH=/rootfs/scen ldd "$bin" >&2
			exit 1
		fi
		{ LD_LIBRARY_PATH=/rootfs/scen ldd "$bin" 2>/dev/null || true; } | tr " " "\n" | sed "s/^[[:space:]]*//" | grep "^/" | sort -u |
			while read -r lib; do
				mkdir -p "/rootfs$(dirname "$lib")"
				cp -L "$lib" "/rootfs$lib"
			done || true
	done
' >/dev/null

cat >"$work/root/init" <<'INIT'
#!/bin/busybox sh
# busybox --install is not reliable across busybox versions (1.30 rejects
# its own argv[0]), so applets are invoked through $BB explicitly.
BB=/bin/busybox
export LD_LIBRARY_PATH=/scen
$BB mount -t proc proc /proc
$BB mount -t sysfs sysfs /sys
$BB mount -t devtmpfs devtmpfs /dev 2>/dev/null || true
$BB mount -t tmpfs tmpfs /tmp

echo "=== etrace kernel tests on $($BB uname -r) ==="
if [ ! -e /sys/kernel/btf/vmlinux ]; then
	echo "ETRACE_KERNEL_TEST_SETUP_FAIL: /sys/kernel/btf/vmlinux is missing"
	$BB reboot -f
fi

export ETRACE_SCENARIO_LOCKCHAIN=/scen/lockchain-amd64
export ETRACE_SCENARIO_NOFP=/scen/nofp-amd64
export ETRACE_SCENARIO_TRACED=/scen/traced-amd64
export ETRACE_SCENARIO_GPU=/scen/gpu-amd64
if [ -s /etc/gtest_filter ]; then
	GTEST_FILTER="$($BB cat /etc/gtest_filter)"
	export GTEST_FILTER
fi

/kt --gtest_color=no
rc=$?
if [ "$rc" -ne 0 ]; then
	echo "--- dmesg tail ---"
	$BB dmesg | $BB tail -n 60
fi
echo "ETRACE_KERNEL_TEST_EXIT=$rc"
$BB sync
$BB reboot -f
INIT
chmod +x "$work/root/init"

echo "==> building initramfs"
(
	cd "$work/root"
	find . -print0 | cpio -0 -o -H newc 2>/dev/null
) | gzip -9 >"$work/initramfs.cpio.gz"
echo "==> initramfs $(du -h "$work/initramfs.cpio.gz" | cut -f1)"

accel=(-accel tcg -cpu max)
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
	accel=(-accel kvm -cpu host)
fi

echo "==> booting qemu (${accel[*]}, smp ${smp}, timeout ${timeout_s}s)"
set +e
timeout "$timeout_s" qemu-system-x86_64 \
	-machine q35 -m 2048 -smp "$smp" -nographic -no-reboot \
	-kernel "$vmlinuz" -initrd "$work/initramfs.cpio.gz" \
	-append "console=ttyS0 rdinit=/init panic=1 loglevel=4" \
	"${accel[@]}" 2>&1 | tee "$work/console.log"
qemu_status=${PIPESTATUS[0]}
set -e

if grep -q 'ETRACE_KERNEL_TEST_SETUP_FAIL' "$work/console.log"; then
	grep 'ETRACE_KERNEL_TEST_SETUP_FAIL' "$work/console.log" >&2
	exit 1
fi
if ! grep -q 'ETRACE_KERNEL_TEST_EXIT=' "$work/console.log"; then
	echo "--- console tail ---" >&2
	tail -n 120 "$work/console.log" >&2
	die "the guest did not reach the tests (qemu status $qemu_status)"
fi
rc=$(sed -n 's/^ETRACE_KERNEL_TEST_EXIT=//p' "$work/console.log" | tr -d '\r' | tail -n 1)
case "$rc" in
'' | *[!0-9]*) die "guest returned an invalid exit status: '$rc'" ;;
esac
if grep -q '\[  SKIPPED \]' "$work/console.log"; then
	die "the suite skipped tests; a missing prerequisite must fail the job"
fi
if [ "$rc" -ne 0 ]; then
	echo "--- console tail ---" >&2
	tail -n 160 "$work/console.log" >&2
	die "kernel tests failed (guest exit $rc)"
fi

grep -E '^\[(  PASSED|==========)' "$work/console.log" | tail -n 3 || true
echo "==> kernel tests passed on $(basename "$vmlinuz")"
