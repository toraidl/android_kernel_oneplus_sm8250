// SPDX-License-Identifier: GPL-2.0-only
/*
 * Persist early kernel messages to the OnePlus kernel_log partition.
 *
 * The SM8250 OnePlus Android 11 vendor driver uses a 4 KiB header followed by
 * seven 1 MiB circular slots.  This implementation keeps that geometry and
 * the OPLOG slot marker, but uses the public kmsg dumper API instead of
 * reaching into printk's private ring-buffer structures.
 */

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kmsg_dump.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>

#include <soc/oplus/system/boot_mode.h>

#include "oplus_kernel_log_v2.h"
#include "oplus_phoenix.h"

#define OPKLOG_RETRY_COUNT		60
#define OPKLOG_RETRY_MSEC		500
#define OPKLOG_POLL_MSEC		500
#define OPKLOG_TIMEOUT_SEC		180
#define OPKLOG_LINE_SIZE		2048

#define OPKLOG_LOGICAL_MAGIC_OFFSET	64U

struct opklog_context {
	struct block_device *bdev;
	char *header_page;
	char *page;
	char *line;
	struct kmsg_dumper dumper;
	u32 boot_count;
	u32 active_slot;
	u32 page_index;
	u32 page_offset;
	u32 payload_len;
	u32 payload_crc;
	bool page_dirty;
	bool slot_full;
};

static struct task_struct *opklog_task;

static int opklog_bio_rw(struct block_device *bdev, void *buffer,
			 size_t size, u64 offset, bool write)
{
	struct bio *bio;
	int ret;

	if (!bdev || !buffer || !size || (offset & (OPKLOG_PAGE_SIZE - 1)) ||
	    (size & (OPKLOG_PAGE_SIZE - 1)))
		return -EINVAL;

	bio = bio_map_kern(bdev_get_queue(bdev), buffer, size, GFP_KERNEL);
	if (IS_ERR(bio))
		return PTR_ERR(bio);

	bio_set_dev(bio, bdev);
	bio->bi_iter.bi_sector = offset >> 9;
	bio->bi_opf = (write ? REQ_OP_WRITE : REQ_OP_READ) | REQ_SYNC;
	ret = submit_bio_wait(bio);
	bio_put(bio);

	return ret;
}

static int opklog_flush(struct block_device *bdev)
{
	int ret;

	ret = blkdev_issue_flush(bdev, GFP_KERNEL, NULL);
	if (ret == -EOPNOTSUPP)
		ret = 0;
	return ret;
}

static struct block_device *opklog_get_bdev(void)
{
	struct block_device *bdev;
	dev_t dev;
	int retry;

	for (retry = 0; retry < OPKLOG_RETRY_COUNT; retry++) {
		dev = name_to_dev_t("PARTLABEL=kernel_log");
		if (dev) {
			bdev = blkdev_get_by_dev(dev, FMODE_READ | FMODE_WRITE, NULL);
			if (!IS_ERR(bdev))
				return bdev;
		}

		msleep(OPKLOG_RETRY_MSEC);
	}

	return NULL;
}

static u32 opklog_header_crc(const struct opklog_header_v2 *header)
{
	struct opklog_header_v2 copy;

	memcpy(&copy, header, sizeof(copy));
	copy.header_crc = 0;
	return ~crc32(~0, (const u8 *)&copy, sizeof(copy));
}

static bool opklog_header_valid(const char *page)
{
	const struct opklog_header_v2 *header;

	header = (const struct opklog_header_v2 *)(page + OPKLOG_V2_HEADER_OFFSET);
	if (memcmp(header->magic, OPKLOG_V2_MAGIC, OPKLOG_V2_MAGIC_SIZE))
		return false;
	if (le16_to_cpu(header->version) != OPKLOG_V2_VERSION ||
	    le16_to_cpu(header->size) != sizeof(*header))
		return false;
	if (le32_to_cpu(header->slot_count) != OPKLOG_SLOT_COUNT ||
	    le32_to_cpu(header->slot_size) != OPKLOG_SLOT_SIZE)
		return false;
	if (!le32_to_cpu(header->header_crc))
		return false;

	return le32_to_cpu(header->header_crc) == opklog_header_crc(header);
}

static void opklog_set_slot_marker(char *page, u32 slot)
{
	page[0] = '\n';
	page[1] = 'O';
	page[2] = 'P';
	page[3] = 'L';
	page[4] = 'O';
	page[5] = 'G';
	page[6] = '0' + slot;
	page[7] = '\n';
}

static void opklog_fill_header(struct opklog_context *ctx, u32 state)
{
	struct opklog_header_v2 *header;

	header = (struct opklog_header_v2 *)(ctx->header_page +
					     OPKLOG_V2_HEADER_OFFSET);
	memcpy(header->magic, OPKLOG_V2_MAGIC, OPKLOG_V2_MAGIC_SIZE);
	header->version = cpu_to_le16(OPKLOG_V2_VERSION);
	header->size = cpu_to_le16(sizeof(*header));
	header->boot_count = cpu_to_le32(ctx->boot_count);
	header->active_slot = cpu_to_le32(ctx->active_slot);
	header->payload_len = cpu_to_le32(ctx->payload_len);
	header->state = cpu_to_le32(state);
	header->slot_count = cpu_to_le32(OPKLOG_SLOT_COUNT);
	header->slot_size = cpu_to_le32(OPKLOG_SLOT_SIZE);
	header->payload_crc = cpu_to_le32(~ctx->payload_crc);
	header->header_crc = 0;
	header->reserved = 0;
	header->header_crc = cpu_to_le32(opklog_header_crc(header));

	memcpy(ctx->header_page + OPKLOG_LOGICAL_MAGIC_OFFSET,
	       OPKLOG_LOGICAL_MAGIC, OPKLOG_LOGICAL_MAGIC_SIZE);
	ctx->header_page[OPKLOG_LOGICAL_MAGIC_OFFSET +
			OPKLOG_LOGICAL_MAGIC_SIZE] = '\0';
	ctx->header_page[OPKLOG_LEGACY_MAGIC_SIZE] = ctx->boot_count & 0xff;
}

static int opklog_checkpoint(struct opklog_context *ctx, u32 state)
{
	opklog_fill_header(ctx, state);
	return opklog_bio_rw(ctx->bdev, ctx->header_page, OPKLOG_HEADER_SIZE,
			     0, true);
}

static u64 opklog_slot_offset(const struct opklog_context *ctx)
{
	return OPKLOG_SLOT_OFFSET +
		(u64)ctx->active_slot * OPKLOG_SLOT_SIZE +
		(u64)ctx->page_index * OPKLOG_PAGE_SIZE;
}

static int opklog_write_page(struct opklog_context *ctx)
{
	if (ctx->page_index >= OPKLOG_SLOT_SIZE / OPKLOG_PAGE_SIZE)
		return -ENOSPC;

	return opklog_bio_rw(ctx->bdev, ctx->page, OPKLOG_PAGE_SIZE,
			     opklog_slot_offset(ctx), true);
}

static int opklog_write_current_page(struct opklog_context *ctx)
{
	int ret;

	if (!ctx->page_dirty)
		return 0;

	ret = opklog_write_page(ctx);
	if (!ret)
		ctx->page_dirty = false;
	return ret;
}

static int opklog_append(struct opklog_context *ctx, const char *line,
			 size_t len)
{
	size_t copied = 0;
	size_t copy_len;
	size_t available;

	while (copied < len && !ctx->slot_full) {
		if (ctx->page_index >= OPKLOG_SLOT_SIZE / OPKLOG_PAGE_SIZE) {
			ctx->slot_full = true;
			break;
		}

		available = OPKLOG_PAGE_SIZE - ctx->page_offset;
		copy_len = min(available, len - copied);
		memcpy(ctx->page + ctx->page_offset, line + copied, copy_len);
		ctx->payload_crc = crc32(ctx->payload_crc,
					 (const u8 *)line + copied, copy_len);
		ctx->payload_len += copy_len;
		ctx->page_offset += copy_len;
		ctx->page_dirty = true;
		copied += copy_len;

		if (ctx->page_offset == OPKLOG_PAGE_SIZE) {
			if (opklog_write_current_page(ctx))
				return -EIO;
			ctx->page_index++;
			ctx->page_offset = 0;
			if (ctx->page_index >= OPKLOG_SLOT_SIZE / OPKLOG_PAGE_SIZE) {
				ctx->slot_full = true;
				break;
			}
			memset(ctx->page, 0, OPKLOG_PAGE_SIZE);
		}
	}

	return 0;
}

static int opklog_prepare(struct opklog_context *ctx)
{
	const struct opklog_header_v2 *old_header;
	u32 previous_count = 0;
	bool has_previous = false;
	bool valid;
	int ret;

	valid = opklog_header_valid(ctx->header_page);
	old_header = (const struct opklog_header_v2 *)(ctx->header_page +
						      OPKLOG_V2_HEADER_OFFSET);
	if (valid) {
		has_previous = true;
		previous_count = le32_to_cpu(old_header->boot_count);
		if (le32_to_cpu(old_header->state) == OPKLOG_STATE_IN_PROGRESS)
			pr_info("oplus kernel_log: previous boot ended before completion\n");
	} else if (!memcmp(ctx->header_page, OPKLOG_LEGACY_MAGIC,
				   OPKLOG_LEGACY_MAGIC_SIZE)) {
		has_previous = true;
		previous_count = (u8)ctx->header_page[OPKLOG_LEGACY_MAGIC_SIZE];
	}

	ctx->boot_count = has_previous ? previous_count + 1 : 0;
	if (!ctx->boot_count && has_previous)
		ctx->boot_count = 1;
	ctx->active_slot = ctx->boot_count % OPKLOG_SLOT_COUNT;
	ctx->payload_len = 0;
	ctx->payload_crc = ~0;
	ctx->page_index = 0;
	ctx->page_offset = OPKLOG_SLOT_PAYLOAD_OFFSET;
	ctx->page_dirty = false;
	ctx->slot_full = false;

	memcpy(ctx->header_page, OPKLOG_LEGACY_MAGIC, OPKLOG_LEGACY_MAGIC_SIZE);
	ret = opklog_checkpoint(ctx, OPKLOG_STATE_IN_PROGRESS);
	if (ret)
		return ret;

	memset(ctx->page, 0, OPKLOG_PAGE_SIZE);
	opklog_set_slot_marker(ctx->page, ctx->active_slot);
	ctx->page_dirty = true;
	ret = opklog_write_current_page(ctx);
	if (ret)
		return ret;

	return opklog_flush(ctx->bdev);
}

static int opklog_record(struct opklog_context *ctx)
{
	unsigned long deadline;
	bool progressed;
	bool completed = false;
	size_t len;
	int ret;

	ctx->dumper.active = true;
	kmsg_dump_rewind(&ctx->dumper);
	deadline = jiffies + OPKLOG_TIMEOUT_SEC * HZ;

	while (!kthread_should_stop() && !ctx->slot_full &&
	       time_before(jiffies, deadline)) {
		progressed = false;
		while (!ctx->slot_full &&
		       kmsg_dump_get_line(&ctx->dumper, true, ctx->line,
					  OPKLOG_LINE_SIZE, &len)) {
			if (!len)
				continue;
			ret = opklog_append(ctx, ctx->line, len);
			if (ret)
				return ret;
			progressed = true;
		}

		if (progressed) {
			ret = opklog_write_current_page(ctx);
			if (ret)
				return ret;
			ret = opklog_checkpoint(ctx, OPKLOG_STATE_IN_PROGRESS);
			if (ret)
				return ret;
			ret = opklog_flush(ctx->bdev);
			if (ret)
				return ret;
		}

		if (phx_is_system_server_init_start()) {
			completed = true;
			break;
		}

		if (msleep_interruptible(OPKLOG_POLL_MSEC) &&
		    kthread_should_stop())
			break;
	}

	ret = opklog_write_current_page(ctx);
	if (ret)
		return ret;
	ret = opklog_checkpoint(ctx, completed ? OPKLOG_STATE_CLOSED :
					 OPKLOG_STATE_IN_PROGRESS);
	if (ret)
		return ret;

	return opklog_flush(ctx->bdev);
}

static int opklog_thread_fn(void *data)
{
	struct opklog_context ctx;
	struct block_device *bdev;
	u64 partition_size;
	unsigned int logical_block_size;
	int ret;

	memset(&ctx, 0, sizeof(ctx));

	bdev = opklog_get_bdev();
	if (!bdev) {
		pr_info("oplus kernel_log: PARTLABEL=kernel_log not found\n");
		return 0;
	}
	ctx.bdev = bdev;
	if (!bdev->bd_part) {
		pr_err("oplus kernel_log: kernel_log has no partition metadata\n");
		ret = -ENODEV;
		goto out_bdev;
	}

	partition_size = (u64)bdev->bd_part->nr_sects << 9;
	logical_block_size = bdev_logical_block_size(bdev);
	if (partition_size < OPKLOG_REQUIRED_SIZE ||
	    logical_block_size > OPKLOG_PAGE_SIZE ||
	    OPKLOG_PAGE_SIZE % logical_block_size) {
		pr_err("oplus kernel_log: unsupported partition size=%llu block=%u\n",
		       partition_size, logical_block_size);
		ret = -EINVAL;
		goto out_bdev;
	}

	ctx.header_page = kzalloc(OPKLOG_HEADER_SIZE, GFP_KERNEL);
	ctx.page = kzalloc(OPKLOG_PAGE_SIZE, GFP_KERNEL);
	ctx.line = kzalloc(OPKLOG_LINE_SIZE, GFP_KERNEL);
	if (!ctx.header_page || !ctx.page || !ctx.line) {
		ret = -ENOMEM;
		goto out_memory;
	}

	ret = opklog_bio_rw(bdev, ctx.header_page, OPKLOG_HEADER_SIZE, 0,
			    false);
	if (ret)
		goto out_memory;

	ret = opklog_prepare(&ctx);
	if (ret)
		goto out_memory;

	pr_info("oplus kernel_log: recording slot=%u boot=%u\n",
		ctx.active_slot, ctx.boot_count);
	ret = opklog_record(&ctx);
	if (ret)
		pr_err("oplus kernel_log: recording stopped: %d\n", ret);

out_memory:
	kfree(ctx.line);
	kfree(ctx.page);
	kfree(ctx.header_page);
out_bdev:
	blkdev_put(bdev, FMODE_READ | FMODE_WRITE);
	return ret;
}

static int __init opklog_init(void)
{
	int boot_mode;

	boot_mode = get_boot_mode();
	if (boot_mode == MSM_BOOT_MODE__FASTBOOT ||
	    boot_mode == MSM_BOOT_MODE__RECOVERY) {
		pr_info("oplus kernel_log: disabled in boot mode %d\n", boot_mode);
		return 0;
	}

	opklog_task = kthread_run(opklog_thread_fn, NULL, "oplus_kernel_log");
	if (IS_ERR(opklog_task)) {
		pr_err("oplus kernel_log: failed to start thread: %ld\n",
		       PTR_ERR(opklog_task));
		opklog_task = NULL;
	}

	return 0;
}

late_initcall(opklog_init);

MODULE_LICENSE("GPL v2");
