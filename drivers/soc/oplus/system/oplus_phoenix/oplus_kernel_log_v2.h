/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OnePlus kernel_log on SM8250 devices uses seven 1 MiB records after a
 * 4 KiB partition header.  Keep the legacy marker and slot offsets intact so
 * logs can still be inspected by the bootloader and recovery tools.
 */
#ifndef _OPLUS_KERNEL_LOG_V2_H
#define _OPLUS_KERNEL_LOG_V2_H

#include <linux/types.h>

#define OPKLOG_PAGE_SIZE			4096U
#define OPKLOG_HEADER_SIZE			OPKLOG_PAGE_SIZE
#define OPKLOG_SLOT_SIZE			(256U * OPKLOG_PAGE_SIZE)
#define OPKLOG_SLOT_COUNT			7U
#define OPKLOG_SLOT_OFFSET			OPKLOG_HEADER_SIZE
#define OPKLOG_SLOT_PAYLOAD_OFFSET		8U
#define OPKLOG_PAYLOAD_SIZE			(OPKLOG_SLOT_SIZE - \
					 OPKLOG_SLOT_PAYLOAD_OFFSET)
#define OPKLOG_REQUIRED_SIZE			(OPKLOG_SLOT_OFFSET + \
					 OPKLOG_SLOT_COUNT * OPKLOG_SLOT_SIZE)

/* The vendor driver writes only the first seven bytes of OPKERNELLOG. */
#define OPKLOG_LEGACY_MAGIC			"OPKERNE"
#define OPKLOG_LEGACY_MAGIC_SIZE		7U
#define OPKLOG_LOGICAL_MAGIC			"OPKERNELLOG"
#define OPKLOG_LOGICAL_MAGIC_SIZE		11U

#define OPKLOG_SLOT_MARKER_SIZE		8U
#define OPKLOG_V2_HEADER_OFFSET		16U
#define OPKLOG_V2_MAGIC			"OPKLOGV2"
#define OPKLOG_V2_MAGIC_SIZE			8U
#define OPKLOG_V2_VERSION			1U

#define OPKLOG_STATE_IN_PROGRESS		1U
#define OPKLOG_STATE_CLOSED			2U

/*
 * This extension is deliberately outside the vendor's eight-byte slot
 * prefix.  Payload bytes therefore start at the same offset as the original
 * OnePlus driver, while recovery can use the length and CRC to ignore stale
 * data left at the end of a reused slot.
 */
struct opklog_header_v2 {
	char magic[OPKLOG_V2_MAGIC_SIZE];
	__le16 version;
	__le16 size;
	__le32 boot_count;
	__le32 active_slot;
	__le32 payload_len;
	__le32 state;
	__le32 slot_count;
	__le32 slot_size;
	__le32 payload_crc;
	__le32 header_crc;
	__le32 reserved;
} __packed;

#endif /* _OPLUS_KERNEL_LOG_V2_H */
