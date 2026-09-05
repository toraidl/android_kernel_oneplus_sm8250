#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# hang_debug_extract.sh — read-only post-reboot extraction for SM8250 hang diagnostics
# Usage: sh hang_debug_extract.sh [outdir]
# Works from Android root shell or recovery (requires /dev/block/by-name)
# Never writes to partitions; uses dd iflag=direct read-only, records sha256 or md5

set -eu

OUTDIR="${1:-/data/media/0/hang_debug}"
TS="$(date +%Y%m%d-%H%M%S 2>/dev/null || echo unknown)"
OUTDIR="${OUTDIR}/${TS}"
HASH_CMD=""

if command -v sha256sum >/dev/null 2>&1; then
	HASH_CMD="sha256sum"
elif command -v sha1sum >/dev/null 2>&1; then
	HASH_CMD="sha1sum"
elif command -v md5sum >/dev/null 2>&1; then
	HASH_CMD="md5sum"
else
	HASH_CMD="md5"
fi

mkdir -p "$OUTDIR" 2>/dev/null || OUTDIR="/tmp/hang_debug/${TS}"; mkdir -p "$OUTDIR"

log() { echo "[hang_extract] $*" | tee -a "$OUTDIR/extract.log"; }

log "outdir=$OUTDIR ts=$TS hash=$HASH_CMD"
log "ro.build.version.incremental=$(getprop ro.build.version.incremental 2>/dev/null || echo unknown)"
log "uname=$(uname -a 2>/dev/null || cat /proc/version 2>/dev/null || echo unknown)"

# helper: read-only dd with hash
dump_part() {
	name="$1"  # e.g. oplusreserve5
	out="$2"   # filename
	limit="${3:-}" # optional count*bs
	part="/dev/block/by-name/${name}"
	if [ ! -e "$part" ]; then
		# fallback opporeserve naming
		if [ -e "/dev/block/by-name/opporeserve5" ] && [ "$name" = "oplusreserve5" ]; then
			part="/dev/block/by-name/opporeserve5"
		elif [ -e "/dev/block/bootdevice/by-name/${name}" ]; then
			part="/dev/block/bootdevice/by-name/${name}"
		else
			log "skip $name: not found $part"
			return 0
		fi
	fi
	log "dumping $name -> $out via $part"
	# use 1M blocks, 64M for oplusreserve5, else full
	if [ -n "$limit" ]; then
		dd if="$part" of="$OUTDIR/$out" bs=1M count="$limit" iflag=direct 2>>"$OUTDIR/extract.log" || \
		dd if="$part" of="$OUTDIR/$out" bs=4096 2>>"$OUTDIR/extract.log" || log "dd $name failed"
	else
		dd if="$part" of="$OUTDIR/$out" bs=1M iflag=direct 2>>"$OUTDIR/extract.log" || \
		dd if="$part" of="$OUTDIR/$out" bs=4096 2>>"$OUTDIR/extract.log" || log "dd $name failed"
	fi
	if [ -f "$OUTDIR/$out" ]; then
		ls -lh "$OUTDIR/$out" >>"$OUTDIR/extract.log" 2>&1 || true
		$HASH_CMD "$OUTDIR/$out" >>"$OUTDIR/SHA256SUM" 2>/dev/null || $HASH_CMD "$OUTDIR/$out" >>"$OUTDIR/hashes.txt" 2>&1 || true
		log "done $name $(stat -c%s "$OUTDIR/$out" 2>/dev/null || wc -c <"$OUTDIR/$out") bytes"
	fi
}

# 1. oplusreserve partitions (KMSG_WB phoenix2.0 is oplusreserve5 64M)
dump_part oplusreserve1 oplusreserve1.img 1
dump_part oplusreserve2 oplusreserve2.img 1
dump_part oplusreserve3 oplusreserve3.img 1
dump_part oplusreserve5 oplusreserve5.img 64
dump_part kernel_log kernel_log.img
# fallback names
dump_part opporeserve1 opporeserve1.img 1
dump_part reserve3 reserve3.img 1

# 2. pstore / ramoops via pstorefs (kona ramoops@0xB0000000 4M)
if [ -d /sys/fs/pstore ]; then
	log "pstorefs found, copying"
	mkdir -p "$OUTDIR/pstore"
	cp -a /sys/fs/pstore/* "$OUTDIR/pstore/" 2>>"$OUTDIR/extract.log" || log "pstore copy partial"
	ls -R /sys/fs/pstore >>"$OUTDIR/extract.log" 2>&1 || true
	$HASH_CMD "$OUTDIR/pstore"/* >>"$OUTDIR/SHA256SUM" 2>/dev/null || true
else
	log "no /sys/fs/pstore (ramoops backend missing or not mounted)"
fi
# pmsg is via /dev/pmsg0, not needed for dump

# 3. minidump: Qualcomm HLOS minidump is exposed via /sys/kernel/dload or via dump via warm reset
# The HLOS table is in SMEM 602, dump is collected by bootloader on DLOAD. On live system we can
# at least dump the HANGLOG region via /proc/kmsg or via our debugfs
# Provide live HANGLOG via debugfs if available (not persistent across reboot, but shows current)
if [ -f /sys/kernel/debug/hang_debug/trigger ]; then
	log "live hang_debug debugfs present"
	cat /sys/kernel/debug/hang_debug/* 2>>"$OUTDIR/extract.log" || true
fi
# Also try to dump kernel log buffer via /proc/kmsg is streaming, use dmesg
log "capturing dmesg"
dmesg >"$OUTDIR/dmesg.txt" 2>>"$OUTDIR/extract.log" || log "dmesg failed"
$HASH_CMD "$OUTDIR/dmesg.txt" >>"$OUTDIR/SHA256SUM" 2>/dev/null || true

# 4. HANGLOG / KLOGBUF via minidump: on Qualcomm devices after warm reset, the dump is in separate
# dump partition (often /dev/block/by-name/dump or via /dev/block/sdaXX). We try to locate.
for part in dump rawdump minidump oplus_dump; do
	if [ -e "/dev/block/by-name/$part" ]; then
		dump_part "$part" "${part}.img" 64
	fi
done
# 5. TRACEBUF is not yet implemented as separate region; HANGLOG already contains trace freeze marker
log "TRACEBUF: not separate region, HANGLOG contains trace freeze flag"

# 6. KLOGBUF is KLOGBUF minidump region; extraction same as dump partition. Also try /proc/last_kmsg if exists
if [ -f /proc/last_kmsg ]; then
	cp /proc/last_kmsg "$OUTDIR/last_kmsg.txt" 2>>"$OUTDIR/extract.log" || true
	$HASH_CMD "$OUTDIR/last_kmsg.txt" >>"$OUTDIR/SHA256SUM" 2>/dev/null || true
fi
if [ -f /sys/module/printk/parameters/* ]; then
	log "printk params"
	cat /sys/module/printk/parameters/* 2>>"$OUTDIR/extract.log" || true
fi

# 7. overall hashes and manifest
log "final manifest"
ls -lhR "$OUTDIR" >>"$OUTDIR/extract.log" 2>&1 || true
if [ -f "$OUTDIR/SHA256SUM" ]; then
	cat "$OUTDIR/SHA256SUM" >>"$OUTDIR/extract.log"
fi
log "done outdir=$OUTDIR"
echo "DONE $OUTDIR" | tee -a "$OUTDIR/extract.log"
# Never erase partitions; read-only only
