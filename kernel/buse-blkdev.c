/* Copyright (C) 2021-2022 Vojtech Aschenbrenner <v@asch.cz> */

#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "buse-blkdev.h"
#include "buse-rqueue.h"
#include "buse-wqueue.h"
#include "main.h"

// https://www.kernel.org/doc/html/v5.15/block/blk-mq.html

/*
 * Init function called for every hardware queue of created device. We just fill user data and compute the
 * queue id. 
 */
static int buse_init_hctx(struct blk_mq_hw_ctx *hw_ctx, void *driver_data, unsigned int hw_ctx_id)
{
	printk(KERN_DEBUG "buse: buse_init_hctx called");
	struct buse *buse = hw_ctx->queue->queuedata;
	struct buse_queue *q = buse->queues;

	q[hw_ctx_id].id = hw_ctx_id;
	hw_ctx->driver_data = &q[hw_ctx_id];
	buse->num_queues++;

	return 0;
}

/*
 * io request callback called for every io in the queue. This function is called by blk-mq.
 */
static blk_status_t buse_queue_rq(struct blk_mq_hw_ctx *hw_ctx, const struct blk_mq_queue_data *data)
{
	struct request *r = data->rq;
	struct buse_cmd *cmd = blk_mq_rq_to_pdu(r);
	struct buse *buse;

	cmd->rq = r;
	cmd->queue = hw_ctx->driver_data;
	cmd->canceled = false;
	cmd->magic = BUSE_MAGIC;

	buse = cmd->queue->r.buse;
	if (atomic_read(&buse->stopped) == 1)
		return BLK_STS_IOERR;

	switch (req_op(r)) {
		case REQ_OP_DISCARD:
		case REQ_OP_WRITE_SAME:
		case REQ_OP_WRITE_ZEROES:
		case REQ_OP_SECURE_ERASE:
		case REQ_OP_WRITE:
			return buse_write(cmd);
		case REQ_OP_FLUSH:
			return buse_flush(cmd);
		case REQ_OP_READ:
			return buse_read(cmd);
	}

	pr_warn("Unsupported request no. %d\n", req_op(r));

	return BLK_STS_IOERR;
}

static const struct block_device_operations buse_blkdev_ops = {
	.owner = THIS_MODULE,
};

/*
 * When io request times out we just print warning to the dmesg a give it another chance. This is
 * the best we can do. If the device is eventually stopped, these requests will be canceled.
 */
static enum blk_eh_timer_return buse_timeout(struct request *rq, bool b)
{
	pr_warn("Request timed out! Is userspace connected? (rq = %p)\n", rq);

	return BLK_EH_RESET_TIMER;
}

/*
 * Control structure for blk-mq operations.
 */
static const struct blk_mq_ops buse_mq_ops = {
	.init_hctx = buse_init_hctx,
	.queue_rq = buse_queue_rq,
	.timeout = buse_timeout,
};

/*
 * blk-mq tags initialization.
 */
static void buse_set_tag_set(struct buse *buse)
{
	struct blk_mq_tag_set *tag_set = &buse->blkdev.tag_set;

	tag_set->cmd_size = sizeof(struct buse_cmd);
	tag_set->driver_data = buse;
	tag_set->flags = BLK_MQ_F_SHOULD_MERGE | BLK_MQ_F_BLOCKING;
	if (buse->no_scheduler)
		tag_set->flags |= BLK_MQ_F_NO_SCHED;
	tag_set->nr_hw_queues = buse->hw_queues;
	tag_set->numa_node = NUMA_NO_NODE;
	tag_set->ops = &buse_mq_ops;
	tag_set->queue_depth = buse->queue_depth;
}

/*
 * Block device initialization. All configuration parameters are set according to the configured
 * values in struct buse. This is only related to the block device side of the module.
 */
int buse_blkdev_init(struct buse *buse)
{
	int ret;
	struct buse_blkdev *blkdev = &buse->blkdev;
	struct blk_mq_tag_set *tag_set = &blkdev->tag_set;
	size_t max_writes = buse->write_chunk_size / buse->block_size;
	size_t writelist_size = max_writes * sizeof(struct writelist_item);
	unsigned int max_hw_sectors;

	buse_set_tag_set(buse);

	ret = blk_mq_alloc_tag_set(tag_set);
	if (ret) {
		printk(KERN_ERR "buse: buse_blkdev_init: blk_mq_alloc_tag_set failed, code %d\n", ret);
		goto err_disk;
	}

	blkdev->disk = blk_mq_alloc_disk(&buse->blkdev.tag_set, buse);
	if (!blkdev->disk) {
		ret = -ENOMEM;
		printk(KERN_ERR "buse: buse_blkdev_init: blk_mq_alloc_disk failed, code %d\n", ret);
		goto err;
	}

	blkdev->request_queue = blkdev->disk->queue;
	if (IS_ERR(blkdev->request_queue)) {
		ret = PTR_ERR(blkdev->request_queue);
		printk(KERN_ERR "buse: queue error, code %d\n", ret);
		goto err_tag;
	}

	blk_queue_write_cache(blkdev->request_queue, true, false);

	max_hw_sectors = (buse->write_chunk_size - writelist_size) / SECTOR_SIZE;
	if (max_hw_sectors > buse->read_shm_size / SECTOR_SIZE)
		max_hw_sectors = buse->read_shm_size / SECTOR_SIZE;
	blk_queue_max_hw_sectors(blkdev->request_queue, max_hw_sectors);

	blk_queue_flag_set(QUEUE_FLAG_NONROT, blkdev->request_queue);
	blk_queue_flag_clear(QUEUE_FLAG_ADD_RANDOM, blkdev->request_queue);
	blk_queue_logical_block_size(blkdev->request_queue, buse->block_size);
	blk_queue_physical_block_size(blkdev->request_queue, buse->block_size);

	if (buse->io_min < buse->block_size || buse->io_min % buse->block_size != 0)
		buse->io_min = buse->block_size;
	blk_queue_io_min(blkdev->request_queue, buse->io_min);

	if (buse->io_opt < buse->block_size || buse->io_opt % buse->block_size != 0)
		buse->io_opt = buse->block_size;
	blk_queue_io_opt(blkdev->request_queue, buse->io_opt);

	blk_queue_max_segments(blkdev->request_queue, USHRT_MAX);
	blk_queue_max_segment_size(blkdev->request_queue, UINT_MAX);

	if (buse->can_write_same)
		blk_queue_max_write_same_sectors(blkdev->request_queue, UINT_MAX);

	if (buse->can_write_zeroes)
		blk_queue_max_write_zeroes_sectors(blkdev->request_queue, UINT_MAX);

	if (buse->can_discard) {
		blk_queue_flag_set(QUEUE_FLAG_DISCARD, blkdev->request_queue);
		blkdev->request_queue->limits.discard_granularity = buse->block_size;
		blkdev->request_queue->limits.discard_alignment = buse->block_size;

		blk_queue_max_discard_sectors(blkdev->request_queue, UINT_MAX);
		blk_queue_max_discard_segments(blkdev->request_queue, USHRT_MAX);
	}

	if (buse->can_secure_erase)
		blk_queue_flag_set(QUEUE_FLAG_SECERASE, blkdev->request_queue);

	blk_queue_virt_boundary(blkdev->request_queue, buse->block_size - 1);

	printk(KERN_DEBUG "buse: init blk disk seems ok");

	return 0;

err_tag:
	blk_mq_free_tag_set(tag_set);
err_disk:
	put_disk(blkdev->disk);
err:
	return ret;
}

/*
 * Remove the block device if it was created, otherwise just cleanup tagset.
 */
void buse_blkdev_exit(struct buse *buse)
{
	if (buse->blkdev.created) {
		del_gendisk(buse->blkdev.disk);
		put_disk(buse->blkdev.disk);
	}

	blk_cleanup_queue(buse->blkdev.request_queue);
	blk_mq_free_tag_set(&buse->blkdev.tag_set);
	buse->blkdev.created = false;
}

/*
 * Registers the block device so that it is visible to the system.
 */
int buse_gendisk_register(struct buse *buse)
{
	struct gendisk *disk = buse->blkdev.disk;
	int ret;

	printk(KERN_DEBUG "buse: initing gendisk");
	disk->major = buse_blkdev_major;
	disk->minors = buse_blkdev_max_minors;
	disk->first_minor = buse->index * disk->minors;
	disk->flags |= GENHD_FL_EXT_DEVT;
	disk->fops = &buse_blkdev_ops;
	disk->private_data = buse;
	disk->queue = buse->blkdev.request_queue;
	snprintf(disk->disk_name, DISK_NAME_LEN, "%s%llu", buse_blkdev_name, buse->index);

	/* Capacity needs to be set to 0, otherwise add_disk() hangs! Correct
	 * capacity is set afterwards. */
	set_capacity(buse->blkdev.disk, 0);
	ret = add_disk(disk);
	if (ret) {
		printk(KERN_ERR "buse: failed to add disk, code = %d", ret);
		goto ERR;
	}
	set_capacity(buse->blkdev.disk, buse->size >> SECTOR_SHIFT);

ERR:
	return ret;
}

/*
 * Returns numa node for given queue id.
 */
int buse_get_numa_node_for_queue_id(struct buse *buse, int queue_id)
{
	int i;
	struct blk_mq_queue_map *qmap = &buse->blkdev.tag_set.map[HCTX_TYPE_DEFAULT];

	for_each_possible_cpu(i) {
		if (queue_id == qmap->mq_map[i])
			return cpu_to_node(i);
	}

	return NUMA_NO_NODE;
}
