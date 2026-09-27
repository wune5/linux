// SPDX-License-Identifier: GPL-2.0
/* Periodically persist the printk ring to a dedicated raw block partition. */

#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/device/driver.h>
#include <linux/fcntl.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kdev_t.h>
#include <linux/kernel.h>
#include <linux/kmsg_dump.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <linux/rubens_earlylog.h>

#define RUBENS_BOOTLOG_MAGIC	0x474f4c42U /* "BLOG" */
#define RUBENS_BOOTLOG_COMMIT	0x54494d43U /* "CMIT" */
#define RUBENS_BOOTLOG_VERSION	1
#define RUBENS_BOOTLOG_SLOT_SIZE	(64 * 1024)
#define RUBENS_BOOTLOG_HEADER_SIZE	4096
#define RUBENS_BOOTLOG_PAYLOAD_SIZE	(RUBENS_BOOTLOG_SLOT_SIZE - \
					 RUBENS_BOOTLOG_HEADER_SIZE)
#define RUBENS_BOOTLOG_SLOTS	2
#define RUBENS_BOOTLOG_PERIOD	(2 * HZ)

struct rubens_bootlog_header {
	__le32 magic;
	__le32 version;
	__le32 generation;
	__le32 payload_len;
	__le32 payload_crc;
	__le32 boot_stage;
	__le32 commit;
	__le32 reserved[1017];
};

static char rubens_bootlog_target[64] = "PARTLABEL=oops";
module_param_string(target, rubens_bootlog_target,
			   sizeof(rubens_bootlog_target), 0400);
MODULE_PARM_DESC(target, "raw block target, e.g. PARTLABEL=oops");

static struct delayed_work rubens_bootlog_work;
static struct file *rubens_bootlog_file;
static unsigned int rubens_bootlog_generation;
static unsigned int rubens_bootlog_stage;
static bool rubens_bootlog_enabled;
static bool rubens_bootlog_previous_saved;

/*
 * Build the bio one page at a time. bio_add_virt_nofail() would describe a
 * slab object that starts mid-page as a single multi-page segment, reading
 * or writing past the end of the object; splitting by page is correct for
 * both kmalloc and vmalloc buffers.
 */
static int rubens_bootlog_rw(struct block_device *bdev, sector_t sector,
			     void *data, size_t len, enum req_op op, gfp_t gfp)
{
	unsigned int nr_vecs = DIV_ROUND_UP(offset_in_page(data) + len,
					    PAGE_SIZE);
	struct bio *bio;
	unsigned int done = 0;
	int ret;

	bio = bio_alloc(bdev, nr_vecs, op | REQ_SYNC, gfp);
	if (!bio)
		return -ENOMEM;
	bio->bi_iter.bi_sector = sector;
	while (done < len) {
		void *addr = (char *)data + done;
		struct page *page = is_vmalloc_addr(addr) ?
			vmalloc_to_page(addr) : virt_to_page(addr);
		unsigned int offset = offset_in_page(addr);
		unsigned int count = min_t(unsigned int, len - done,
					   PAGE_SIZE - offset);

		if (bio_add_page(bio, page, count, offset) != count) {
			ret = -EIO;
			goto out;
		}
		done += count;
	}
	ret = submit_bio_wait(bio);
out:
	bio_put(bio);
	return ret;
}

static int rubens_bootlog_read_header(struct block_device *bdev, unsigned int slot,
					struct rubens_bootlog_header *header)
{
	sector_t sector = (sector_t)slot * RUBENS_BOOTLOG_SLOT_SIZE / 512;

	return rubens_bootlog_rw(bdev, sector, header, sizeof(*header),
				REQ_OP_READ, GFP_KERNEL);
}

static bool rubens_bootlog_header_valid(const struct rubens_bootlog_header *header)
{
	return le32_to_cpu(header->magic) == RUBENS_BOOTLOG_MAGIC &&
		le32_to_cpu(header->version) == RUBENS_BOOTLOG_VERSION &&
		le32_to_cpu(header->commit) == RUBENS_BOOTLOG_COMMIT &&
		le32_to_cpu(header->payload_len) <= RUBENS_BOOTLOG_PAYLOAD_SIZE;
}

static int rubens_bootlog_write(struct block_device *bdev, unsigned int slot,
				const char *payload, size_t payload_len,
				gfp_t gfp)
{
	struct rubens_bootlog_header *header;
	sector_t base = (sector_t)slot * RUBENS_BOOTLOG_SLOT_SIZE / 512;
	void *aligned_payload;
	unsigned int write_len;
	int ret;

	write_len = ALIGN(payload_len, 512);
	if (!payload_len || payload_len > RUBENS_BOOTLOG_PAYLOAD_SIZE ||
	    write_len > RUBENS_BOOTLOG_PAYLOAD_SIZE)
		return -EINVAL;
	aligned_payload = kzalloc(write_len, gfp);
	header = kzalloc(sizeof(*header), gfp);
	if (!aligned_payload || !header) {
		ret = -ENOMEM;
		goto out;
	}

	memcpy(aligned_payload, payload, payload_len);
	ret = rubens_bootlog_rw(bdev,
				base + RUBENS_BOOTLOG_HEADER_SIZE / 512,
				aligned_payload, write_len, REQ_OP_WRITE, gfp);
	if (ret)
		goto out;

	rubens_bootlog_generation++;
	header->magic = cpu_to_le32(RUBENS_BOOTLOG_MAGIC);
	header->version = cpu_to_le32(RUBENS_BOOTLOG_VERSION);
	header->generation = cpu_to_le32(rubens_bootlog_generation);
	header->payload_len = cpu_to_le32(payload_len);
	header->payload_crc = cpu_to_le32(crc32_le(~0, payload, payload_len));
	header->boot_stage = cpu_to_le32(rubens_bootlog_stage);
	/*
	 * Write the final, already-committed header directly, in full 4KB
	 * form. The mainline UFS port returns -EIO for both blkdev_issue_flush
	 * and for a 512-byte write issued right after a failed flush, which
	 * previously kept every record in the uncommitted (commit=0) state.
	 * Each bio completes before the next one is submitted, which is
	 * enough ordering for a warm reboot.
	 */
	header->commit = cpu_to_le32(RUBENS_BOOTLOG_COMMIT);
	ret = rubens_bootlog_rw(bdev, base, header, sizeof(*header),
				REQ_OP_WRITE, gfp);

out:
	kfree(header);
	kfree(aligned_payload);
	return ret;
}

static dev_t rubens_bootlog_devt;
static bool rubens_bootlog_devt_valid;

/*
 * early_lookup_bdev() lives in .init and must not be called once the init
 * sections are freed, so resolve the target once here while they are still
 * mapped.  When the partition is not ready yet, the open falls back to the
 * /dev/disk/by-* symlinks that udev creates after the rootfs is up.
 */
static bool rubens_bootlog_resolve_early(void)
{
	if (rubens_bootlog_devt_valid)
		return true;
	if (early_lookup_bdev(rubens_bootlog_target, &rubens_bootlog_devt))
		return false;
	rubens_bootlog_devt_valid = true;
	return true;
}

static bool rubens_bootlog_target_path(char *buf, size_t len)
{
	const char *t = rubens_bootlog_target;

	if (!strncmp(t, "PARTLABEL=", 10))
		return snprintf(buf, len, "/dev/disk/by-partlabel/%s",
				t + 10) < len;
	if (!strncmp(t, "PARTUUID=", 9))
		return snprintf(buf, len, "/dev/disk/by-partuuid/%s",
				t + 9) < len;
	if (!strncmp(t, "/dev/", 5)) {
		strscpy(buf, t, len);
		return true;
	}
	return false;
}

/*
 * LOCAL PORT CHANGE (rubens-ubuntu, 2026-09-27): resolve the target through
 * sysfs on the failure path.
 *
 * Diagnosed on the device: the oops partition is completely empty after every
 * boot attempt, even though this driver takes a synchronous snapshot from
 * device_initcall_sync and PARTNAME=oops really is the GPT label
 * (/sys/class/block/sdc81/uevent -> PARTNAME=oops).  So the synchronous flush
 * must be failing to open the device.
 *
 * The two existing paths both fail in an initramfs:
 *   early_lookup_bdev()  -- only works once the UFS LUNs and the partition
 *                           scan are done, which is not guaranteed at
 *                           device_initcall_sync time;
 *   /dev/disk/by-partlabel/oops -- a udev symlink, and this port boots an
 *                           initramfs with no udev at all.
 *
 * This adds a third path that needs neither: walk the per-partition "uevent"
 * files under /sys/class/block, looking for PARTNAME=<target>, then read the
 * sibling "dev" attribute for major:minor and build the dev_t with MKDEV().
 * Block devices and their sysfs attributes are registered by the block layer
 * itself, so this works as soon as the partition exists, with no udev and no
 * mounted rootfs.
 */
static bool rubens_bootlog_lookup_partlabel(const char *label, dev_t *out)
{
	static const char *const sysblock = "/sys/class/block";
	struct file *f;
	char buf[256];
	char uevent[256];
	char devpath[128];
	char want[64];
	unsigned int major, minor;
	int i;

	if (strncmp(label, "PARTLABEL=", 10))
		return false;
	snprintf(want, sizeof(want), "PARTNAME=%s", label + 10);

	for (i = 0; i < 128; i++) {
		snprintf(devpath, sizeof(devpath), "%s/sdc%d/uevent", sysblock, i);
		f = filp_open(devpath, O_RDONLY, 0);
		if (IS_ERR(f))
			continue;
		memset(uevent, 0, sizeof(uevent));
		if (kernel_read(f, uevent, sizeof(uevent) - 1, &f->f_pos) > 0) {
			filp_close(f, NULL);
			if (!strstr(uevent, want))
				continue;
		} else {
			filp_close(f, NULL);
			continue;
		}
		snprintf(devpath, sizeof(devpath), "%s/sdc%d/dev", sysblock, i);
		f = filp_open(devpath, O_RDONLY, 0);
		if (IS_ERR(f))
			continue;
		memset(buf, 0, sizeof(buf));
		if (kernel_read(f, buf, sizeof(buf) - 1, &f->f_pos) > 0 &&
		    sscanf(buf, "%u:%u", &major, &minor) == 2) {
			filp_close(f, NULL);
			*out = MKDEV(major, minor);
			pr_info("rubens-bootlog: %s resolved via sysfs to %u:%u\n",
				label, major, minor);
			return true;
		}
		filp_close(f, NULL);
	}
	return false;
}

/*
 * Resolve and open the oops partition lazily: at late_initcall the UFS LUNs
 * may not be registered yet, and a failure there must not disable the panic
 * dumper.  Called from the periodic flush and from the panic path itself.
 */
/*
 * LOCAL PORT CHANGE (rubens-ubuntu, 2026-09-27): last-resort resolution by a
 * fixed partition minor.
 *
 * Both other paths are timing-dependent and both were observed to fail on this
 * device: early_lookup_bdev() needs the asynchronous UFS/SCSI partition scan to
 * have completed, and the sysfs walk needs /sys/class/block to be populated.
 * bdev_file_open_by_dev() is synchronous, so opening by dev_t directly removes
 * every timing dependency.
 *
 * Values read from the live device:
 *   /sys/class/block/sdc81/uevent -> MAJOR=259 MINOR=65 DEVNAME=sdc81
 *   /sys/class/block/sdc81/uevent -> PARTNAME=oops
 *   The device has 88 GPT partitions; sdc81 is the oops partition.  The minor
 *   is allocated in disk discovery order, not by GPT index, so the measured
 *   value (65) is used verbatim rather than derived.
 *
 * The major (259) is the dynamic block major the kernel hands out for
 * partitions; sdc itself is 8:32, i.e. the fixed SCSI disk major.
 *
 * Overridable at boot with rubens_bootlog.devt_major / .devt_minor for other
 * boards; setting both to 0 disables this path.
 */
static unsigned int rubens_bootlog_fixed_major = 259;
static unsigned int rubens_bootlog_fixed_minor = 65;
module_param_named(devt_major, rubens_bootlog_fixed_major, uint, 0400);
module_param_named(devt_minor, rubens_bootlog_fixed_minor, uint, 0400);

static bool rubens_bootlog_resolve_fixed(dev_t *out)
{
	if (!rubens_bootlog_fixed_major && !rubens_bootlog_fixed_minor)
		return false;
	*out = MKDEV(rubens_bootlog_fixed_major, rubens_bootlog_fixed_minor);
	pr_info("rubens-bootlog: falling back to devt %u:%u\n",
		rubens_bootlog_fixed_major, rubens_bootlog_fixed_minor);
	return true;
}

static bool rubens_bootlog_open_bdev(void)
{
	struct rubens_bootlog_header *header;
	dev_t devt;
	unsigned int i;
	char path[96];

	if (rubens_bootlog_file)
		return true;

	if (rubens_bootlog_resolve_early()) {
		devt = rubens_bootlog_devt;
	} else if (rubens_bootlog_lookup_partlabel(rubens_bootlog_target,
						   &devt)) {
		/* got it from sysfs */
	} else if (rubens_bootlog_resolve_fixed(&devt)) {
		/* got it from the fixed partition number */
	} else {
		if (!rubens_bootlog_target_path(path, sizeof(path)) ||
		    lookup_bdev(path, &devt))
			return false;
	}

	rubens_bootlog_file = bdev_file_open_by_dev(devt,
		BLK_OPEN_READ | BLK_OPEN_WRITE, &rubens_bootlog_file, NULL);
	if (IS_ERR(rubens_bootlog_file)) {
		rubens_bootlog_file = NULL;
		return false;
	}

	header = kzalloc(sizeof(*header), GFP_KERNEL);
	if (!header)
		return true;

	for (i = 0; i < RUBENS_BOOTLOG_SLOTS; i++) {
		if (!rubens_bootlog_read_header(file_bdev(rubens_bootlog_file), i,
					     header) &&
		    rubens_bootlog_header_valid(header))
			rubens_bootlog_generation =
				max(rubens_bootlog_generation,
				    le32_to_cpu(header->generation));
	}
	kfree(header);
	pr_info("rubens-bootlog: attached to %s\n", rubens_bootlog_target);
	return true;
}

static void rubens_bootlog_flush(struct work_struct *work)
{
	struct kmsg_dump_iter iter = { };
	struct block_device *bdev;
	char *buffer;
	size_t len;
	unsigned int slot;
	int ret;

	if (!rubens_bootlog_enabled)
		goto reschedule;

	if (!rubens_bootlog_open_bdev())
		goto reschedule;

	buffer = kmalloc(RUBENS_BOOTLOG_PAYLOAD_SIZE, GFP_KERNEL);
	if (!buffer)
		goto reschedule;

	bdev = file_bdev(rubens_bootlog_file);

	/*
	 * The first flush also mirrors the previous early-log ring, if one is
	 * still valid in DRAM. Whatever happens, the current printk snapshot
	 * is written below: the early-log copy must never suppress it. (An
	 * earlier revision zeroed the snapshot length when no previous ring
	 * was available and the write failed with -EINVAL.)
	 */
	if (!rubens_bootlog_previous_saved) {
		size_t previous_len;

		rubens_bootlog_previous_saved = true;
		previous_len = rubens_earlylog_copy_previous(buffer,
							     RUBENS_BOOTLOG_PAYLOAD_SIZE);
		if (previous_len) {
			slot = (rubens_bootlog_generation + 1) %
				RUBENS_BOOTLOG_SLOTS;
			ret = rubens_bootlog_write(bdev, slot, buffer,
						   previous_len, GFP_KERNEL);
			if (ret)
				pr_warn("rubens-bootlog: previous ring write failed: %d\n",
					ret);
			else
				pr_info("rubens-bootlog: previous ring committed to slot %u (%zu bytes)\n",
					slot, previous_len);
		}
	}

	kmsg_dump_rewind(&iter);
	if (!kmsg_dump_get_buffer(&iter, true, buffer,
				  RUBENS_BOOTLOG_PAYLOAD_SIZE, &len))
		goto free_buffer;

	/* rubens_bootlog_write increments the generation before committing. */
	slot = (rubens_bootlog_generation + 1) % RUBENS_BOOTLOG_SLOTS;
	ret = rubens_bootlog_write(bdev, slot, buffer, len, GFP_KERNEL);
	if (ret)
		pr_warn("rubens-bootlog: write slot %u failed: %d\n", slot, ret);
	else
		pr_debug("rubens-bootlog: committed slot %u, %zu bytes, generation %u\n",
			 slot, len, rubens_bootlog_generation);

free_buffer:
	kfree(buffer);
reschedule:
	if (rubens_bootlog_enabled)
		queue_delayed_work(system_dfl_wq, &rubens_bootlog_work,
				   RUBENS_BOOTLOG_PERIOD);
}

/*
 * Panic/OOPS capture: register as a kmsg dumper so the crash snapshot is
 * committed to the oops partition from the panic path itself.  The periodic
 * work only snapshots a live system, so without this a kernel that dies
 * before userspace (e.g. the connectivity bring-up) leaves no trace at all.
 */
static void rubens_bootlog_kmsg_dump(struct kmsg_dumper *dumper,
				     struct kmsg_dump_detail *detail)
{
	struct kmsg_dump_iter iter = { };
	struct block_device *bdev;
	char *buffer;
	size_t len;
	unsigned int slot;
	enum kmsg_dump_reason reason = detail->reason;

	if (!rubens_bootlog_enabled)
		return;
	if (reason != KMSG_DUMP_OOPS && reason != KMSG_DUMP_PANIC &&
	    reason != KMSG_DUMP_EMERG)
		return;
	if (!rubens_bootlog_open_bdev())
		return;

	buffer = kmalloc(RUBENS_BOOTLOG_PAYLOAD_SIZE, GFP_ATOMIC);
	if (!buffer)
		return;

	kmsg_dump_rewind(&iter);
	if (!kmsg_dump_get_buffer(&iter, true, buffer,
				  RUBENS_BOOTLOG_PAYLOAD_SIZE, &len)) {
		kfree(buffer);
		return;
	}

	bdev = file_bdev(rubens_bootlog_file);
	slot = (rubens_bootlog_generation + 1) % RUBENS_BOOTLOG_SLOTS;
	rubens_bootlog_write(bdev, slot, buffer, len, GFP_ATOMIC);
	kfree(buffer);
}

static struct kmsg_dumper rubens_bootlog_kmsg_dumper = {
	.dump = rubens_bootlog_kmsg_dump,
};

static int __init rubens_bootlog_init(void)
{
	/*
	 * Arm the panic dumper first: the oops partition may only appear later
	 * (UFS LUN discovery and SCSI disk registration are asynchronous), and
	 * a crash before that must still not lose the capability to capture.
	 */
	rubens_bootlog_enabled = true;
	INIT_DELAYED_WORK(&rubens_bootlog_work, rubens_bootlog_flush);
	kmsg_dump_register(&rubens_bootlog_kmsg_dumper);

	/* The oops partition is resolved lazily from the flush; do not wait
	 * for the async UFS/SCSI probe here, a stall would hide later hangs. */
	pr_info("rubens-bootlog: panic capture armed for %s\n",
		rubens_bootlog_target);
	rubens_earlylog_stage(16);
	/*
	 * Try to resolve the target now, while the init sections that host
	 * early_lookup_bdev() are still mapped.  Failure is fine: the open
	 * path retries through /dev/disk/by-* after udev has run.
	 */
	rubens_bootlog_resolve_early();

	/*
	 * Take the first snapshot synchronously: running at late_initcall (see
	 * the registration below) puts this in process context with kthreadd,
	 * sysfs and the UFS/SCSI stack all up, and committing a record before
	 * the initcalls return maximizes the chance of capturing a crash early
	 * in userspace.  The flush reschedules itself for the periodic updates.
	 */
	rubens_bootlog_flush(NULL);
	return 0;
}

/*
 * LOCAL PORT CHANGE (rubens-ubuntu, 2026-09-27): registered at late_initcall
 * instead of device_initcall_sync.
 *
 * Measured on the device: the oops partition stays byte-for-byte zero after
 * every boot attempt, i.e. the first (synchronous) flush never commits.  Two
 * things can keep it from working at device_initcall_sync level (initcall
 * level 6):
 *
 *   - the sysfs fallback added above uses filp_open()/kernel_read(), which
 *     need kthreadd and a working sysfs;
 *   - early_lookup_bdev() only succeeds once the SCSI disk and its partitions
 *     have been scanned, which is asynchronous.
 *
 * late_initcall runs at level 7, right after every level-6 driver has probed,
 * so both are satisfied.  The device is observed to live for roughly 4 s and
 * userland is never reached, so this is still early enough to be useful: if
 * the partition is empty again after this change, the kernel is dying before
 * late_initcall, and that is itself the answer.
 */
late_initcall(rubens_bootlog_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Rubens early printk snapshot to a raw block partition");
