// SPDX-License-Identifier: GPL-2.0
/*
 * Debug: keep the kernel log across hangs by writing it to the raw "logdump"
 * partition. RAM (pstore) may not survive a hard reset and /data is not up early
 * in boot, so this only depends on the storage driver.
 *
 * Layout: one header page, then XDLOG_SLOTS equal slots, one per boot, reused
 * in turn. A slot's first XDLOG_HEAD_BYTES keep the start of the boot; the rest
 * is a ring holding the newest lines. Text is plain, so `strings` can read it.
 * tools/xdlog-read.py prints the boots in order.
 *
 * A hard freeze loses at most the last XDLOG_FLUSH_MS of log.
 */

#define pr_fmt(fmt) "xdlog: " fmt

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/freezer.h>
#include <linux/genhd.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kmsg_dump.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/reboot.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>

#define XDLOG_PART		"logdump"
#define XDLOG_MAGIC		0x31474f4c44585858ULL	/* "XXXDLOG1" */
#define XDLOG_SLOTS		8ULL
#define XDLOG_HEAD_BYTES	(1024ULL * 1024)
#define XDLOG_FLUSH_MS		500
#define XDLOG_WAIT_MS		100
#define XDLOG_WAIT_TRIES	600	/* 60 s for the storage driver */
#define XDLOG_MAX_ERRORS	20

struct xdlog_header {
	__le64 magic;
	__le64 boot;
	__le64 slots;
	__le64 slot_bytes;
};

static struct block_device *xd_bdev;
static struct page *xd_page;		/* the slot page being filled */
static char *xd_line;
static u64 xd_slot_start, xd_slot_bytes, xd_pos;
static bool xd_ready;
static unsigned int xd_errors;
static DEFINE_MUTEX(xd_lock);
static struct kmsg_dumper xd_dumper = { .active = true };

static int xd_match_part(struct device *dev, const void *data)
{
	struct hd_struct *part;

	if (dev->type != &part_type)
		return 0;
	part = dev_to_part(dev);
	return part->info && !strcmp(part->info->volname, XDLOG_PART);
}

static int xd_io(unsigned int op, u64 byte_pos, struct page *page)
{
	struct bio *bio;
	int ret;

	bio = bio_alloc(GFP_KERNEL, 1);
	if (!bio)
		return -ENOMEM;
	bio_set_dev(bio, xd_bdev);
	bio->bi_iter.bi_sector = byte_pos >> 9;
	bio->bi_opf = op;
	bio_add_page(bio, page, PAGE_SIZE, 0);
	ret = submit_bio_wait(bio);
	bio_put(bio);
	return ret;
}

static int xd_write_page(void)
{
	/* the tail page is rewritten as it fills, so FUA makes each flush durable */
	return xd_io(REQ_OP_WRITE | REQ_SYNC | REQ_FUA,
		     xd_slot_start + round_down(xd_pos - 1, PAGE_SIZE), xd_page);
}

static void xd_put(const char *s, size_t len)
{
	char *page = page_address(xd_page);

	while (len) {
		size_t off = xd_pos % PAGE_SIZE;
		size_t n = min_t(size_t, len, PAGE_SIZE - off);

		memcpy(page + off, s, n);
		xd_pos += n;
		s += n;
		len -= n;

		if (!(xd_pos % PAGE_SIZE)) {
			if (xd_write_page())
				xd_errors++;
			memset(page, 0, PAGE_SIZE);
			if (xd_pos >= xd_slot_bytes)
				xd_pos = XDLOG_HEAD_BYTES;
		}
	}
}

/* copy new log lines into the slot and make them durable */
static void xd_flush(void)
{
	size_t len;
	bool wrote = false;

	mutex_lock(&xd_lock);
	if (!xd_ready || xd_errors > XDLOG_MAX_ERRORS)
		goto out;
	while (kmsg_dump_get_line(&xd_dumper, true, xd_line, PAGE_SIZE, &len)) {
		xd_put(xd_line, len);
		wrote = true;
	}
	if (wrote && (xd_pos % PAGE_SIZE) && xd_write_page())
		xd_errors++;
out:
	mutex_unlock(&xd_lock);
}

static int xd_reboot_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	/* runs before devices shut down, so storage still works */
	xd_flush();
	return NOTIFY_DONE;
}

static struct notifier_block xd_reboot_nb = {
	.notifier_call = xd_reboot_notify,
	.priority = INT_MIN,	/* last, to include what other notifiers logged */
};

static struct device *xd_wait_for_part(void)
{
	struct device *dev;
	int i;

	for (i = 0; i < XDLOG_WAIT_TRIES; i++) {
		dev = class_find_device(&block_class, NULL, NULL, xd_match_part);
		if (dev)
			return dev;
		msleep(XDLOG_WAIT_MS);
	}
	return NULL;
}

static int xd_open(void)
{
	const fmode_t mode = FMODE_READ | FMODE_WRITE;
	struct xdlog_header *hdr;
	struct page *hdr_page;
	struct device *dev;
	u64 boot = 0, size;
	int ret, n;

	dev = xd_wait_for_part();
	if (!dev) {
		pr_err("no %s partition\n", XDLOG_PART);
		return -ENODEV;
	}
	xd_bdev = blkdev_get_by_dev(dev->devt, mode, THIS_MODULE);
	put_device(dev);
	if (IS_ERR(xd_bdev)) {
		ret = PTR_ERR(xd_bdev);
		xd_bdev = NULL;
		return ret;
	}

	size = i_size_read(xd_bdev->bd_inode);
	xd_slot_bytes = rounddown((size - PAGE_SIZE) / XDLOG_SLOTS, PAGE_SIZE);
	if (xd_slot_bytes <= XDLOG_HEAD_BYTES * 2) {
		ret = -ENOSPC;
		goto put;
	}

	hdr_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!hdr_page) {
		ret = -ENOMEM;
		goto put;
	}
	hdr = page_address(hdr_page);
	ret = xd_io(REQ_OP_READ, 0, hdr_page);
	if (!ret && le64_to_cpu(hdr->magic) == XDLOG_MAGIC)
		boot = le64_to_cpu(hdr->boot) + 1;
	if (ret)
		goto free_hdr;

	memset(hdr, 0, PAGE_SIZE);
	hdr->magic = cpu_to_le64(XDLOG_MAGIC);
	hdr->boot = cpu_to_le64(boot);
	hdr->slots = cpu_to_le64(XDLOG_SLOTS);
	hdr->slot_bytes = cpu_to_le64(xd_slot_bytes);
	ret = xd_io(REQ_OP_WRITE | REQ_SYNC | REQ_FUA, 0, hdr_page);
	if (ret)
		goto free_hdr;

	xd_slot_start = PAGE_SIZE + do_div(boot, XDLOG_SLOTS) * xd_slot_bytes;
	xd_pos = 0;

	/* an older boot's text must not read as part of this one */
	ret = blkdev_issue_zeroout(xd_bdev, xd_slot_start >> 9, xd_slot_bytes >> 9,
				   GFP_KERNEL, 0);
	if (ret)
		goto free_hdr;

	mutex_lock(&xd_lock);
	xd_ready = true;
	mutex_unlock(&xd_lock);

	n = scnprintf(xd_line, PAGE_SIZE, "=== xdlog boot %llu ===\n",
		      le64_to_cpu(hdr->boot));
	kmsg_dump_rewind(&xd_dumper);
	mutex_lock(&xd_lock);
	xd_put(xd_line, n);
	mutex_unlock(&xd_lock);
free_hdr:
	__free_page(hdr_page);
	if (!ret)
		return 0;
put:
	blkdev_put(xd_bdev, mode);
	xd_bdev = NULL;
	return ret;
}

static int xd_thread(void *arg)
{
	int ret = xd_open();

	if (ret) {
		pr_err("disabled: %d\n", ret);
		return ret;
	}
	pr_info("logging to %s, slot at %llu\n", XDLOG_PART, xd_slot_start);
	register_reboot_notifier(&xd_reboot_nb);

	set_freezable();
	set_user_nice(current, -10);
	while (!kthread_should_stop()) {
		xd_flush();
		try_to_freeze();
		schedule_timeout_interruptible(msecs_to_jiffies(XDLOG_FLUSH_MS));
	}
	return 0;
}

static int __init xdlog_init(void)
{
	struct task_struct *t;

	xd_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	xd_line = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!xd_page || !xd_line)
		return -ENOMEM;
	t = kthread_run(xd_thread, NULL, "xdlog");
	return PTR_ERR_OR_ZERO(t);
}
late_initcall(xdlog_init);
