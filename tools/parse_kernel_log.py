#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Decode OnePlus SM8250 kernel_log images.

The parser accepts both the Android 11 vendor layout and the v2 extension
written by the in-kernel logger.  A raw partition dump is never modified.
"""

import argparse
import struct
import sys
from pathlib import Path
import zlib


PAGE_SIZE = 4096
HEADER_SIZE = PAGE_SIZE
SLOT_SIZE = 256 * PAGE_SIZE
SLOT_COUNT = 7
SLOT_OFFSET = HEADER_SIZE
SLOT_PAYLOAD_OFFSET = 8
LEGACY_MAGIC = b"OPKERNE"
LOGICAL_MAGIC = b"OPKERNELLOG"
V2_MAGIC = b"OPKLOGV2"
V2_VERSION = 1
V2_HEADER_OFFSET = 16
V2_HEADER_FORMAT = "<8sHH" + "I" * 9
V2_HEADER_SIZE = struct.calcsize(V2_HEADER_FORMAT)
V2_HEADER_CRC_OFFSET = struct.calcsize("<8sHH") + 7 * 4
STATE_NAMES = {1: "in-progress", 2: "closed"}


def decode_header(page):
    """Return (format_name, metadata) for a 4 KiB header page."""

    metadata = {
        "boot_count": None,
        "active_slot": None,
        "payload_len": None,
        "payload_crc": None,
        "state": None,
        "header_crc_ok": None,
        "slot_count": SLOT_COUNT,
        "slot_size": SLOT_SIZE,
    }

    if len(page) >= 8 and page[: len(LEGACY_MAGIC)] == LEGACY_MAGIC:
        metadata["boot_count"] = page[7]
        metadata["active_slot"] = page[7] % SLOT_COUNT

    if len(page) < V2_HEADER_OFFSET + V2_HEADER_SIZE:
        return "legacy", metadata

    fields = struct.unpack_from(V2_HEADER_FORMAT, page, V2_HEADER_OFFSET)
    magic, version, size = fields[:3]
    if magic != V2_MAGIC or version != V2_VERSION or size != V2_HEADER_SIZE:
        return "legacy", metadata

    (boot_count, active_slot, payload_len, state, slot_count, slot_size,
     payload_crc, header_crc, _reserved) = fields[3:]
    if slot_count != SLOT_COUNT or slot_size != SLOT_SIZE:
        return "legacy", metadata

    extension = bytearray(page[V2_HEADER_OFFSET:V2_HEADER_OFFSET +
                               V2_HEADER_SIZE])
    struct.pack_into("<I", extension, V2_HEADER_CRC_OFFSET, 0)
    crc_ok = header_crc != 0 and (zlib.crc32(extension) & 0xffffffff) == header_crc
    if not crc_ok:
        return "legacy", metadata

    metadata.update({
        "boot_count": boot_count,
        "active_slot": active_slot,
        "payload_len": min(payload_len, SLOT_SIZE - SLOT_PAYLOAD_OFFSET),
        "payload_crc": payload_crc,
        "state": STATE_NAMES.get(state, "unknown(%d)" % state),
        "header_crc_ok": True,
        "slot_count": slot_count,
        "slot_size": slot_size,
    })
    return "opklog-v2", metadata


def marker_slot(page):
    """Return the slot number encoded by an OPLOG marker, or None."""

    if len(page) < 8 or page[0:1] != b"\n" or page[1:6] != b"OPLOG":
        return None
    if page[6:7] not in b"0123456" or page[7:8] != b"\n":
        return None
    return page[6] - ord("0")


def read_slot(stream, slot, slot_size):
    stream.seek(SLOT_OFFSET + slot * slot_size)
    return stream.read(slot_size)


def slot_payload(slot_data, metadata, slot):
    if len(slot_data) <= SLOT_PAYLOAD_OFFSET:
        return b""

    if (metadata["payload_len"] is not None and
            slot == metadata["active_slot"]):
        length = metadata["payload_len"]
        return slot_data[SLOT_PAYLOAD_OFFSET:SLOT_PAYLOAD_OFFSET + length]

    # The original vendor driver has no length field.  Its unused tail is
    # zero-filled, so trimming NULs is the least surprising recovery view.
    return slot_data[SLOT_PAYLOAD_OFFSET:].rstrip(b"\0")


def parse_image(image_path, output_dir=None, selected_slot=None, show=False):
    with image_path.open("rb") as stream:
        header = stream.read(HEADER_SIZE)
        if len(header) < HEADER_SIZE:
            raise ValueError("image is smaller than the 4 KiB header")

        format_name, metadata = decode_header(header)
        print("format=%s" % format_name)
        print("boot_count=%s" % metadata["boot_count"])
        print("active_slot=%s" % metadata["active_slot"])
        if metadata["state"] is not None:
            print("state=%s" % metadata["state"])
        if metadata["header_crc_ok"] is not None:
            print("header_crc=ok")
        if metadata["payload_len"] is not None:
            print("active_payload_len=%d" % metadata["payload_len"])

        destination = Path(output_dir) if output_dir else None
        if destination:
            destination.mkdir(parents=True, exist_ok=True)

        found = 0
        slot_count = metadata["slot_count"]
        slot_size = metadata["slot_size"]
        for slot in range(slot_count):
            if selected_slot is not None and slot != selected_slot:
                continue
            data = read_slot(stream, slot, slot_size)
            if len(data) < slot_size:
                print("slot=%d marker=short-read bytes=%d" % (slot, len(data)))
                continue

            marker = marker_slot(data)
            payload = slot_payload(data, metadata, slot)
            if marker is None and not payload:
                continue

            found += 1
            crc_text = ""
            if (metadata["payload_crc"] is not None and
                    slot == metadata["active_slot"]):
                actual_crc = zlib.crc32(payload) & 0xffffffff
                crc_text = " crc=%s" % ("ok" if actual_crc == metadata["payload_crc"]
                                         else "bad")
            print("slot=%d marker=%s payload=%d%s" %
                  (slot, marker if marker is not None else "none",
                   len(payload), crc_text))

            if destination:
                (destination / ("slot-%d.log" % slot)).write_bytes(payload)
            if show and (selected_slot is None or slot == selected_slot):
                sys.stdout.buffer.write(payload)

        if not found:
            print("no OPLOG slots found")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="raw kernel_log partition dump")
    parser.add_argument("--output-dir", type=Path,
                        help="write decoded slot logs to this directory")
    parser.add_argument("--slot", type=int, choices=range(SLOT_COUNT),
                        help="inspect only one slot")
    parser.add_argument("--show", action="store_true",
                        help="write selected payload bytes to stdout")
    args = parser.parse_args()

    try:
        parse_image(args.image, args.output_dir, args.slot, args.show)
    except (OSError, ValueError, struct.error) as error:
        print("parse_kernel_log.py: %s" % error, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
