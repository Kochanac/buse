/* Copyright (C) 2021-2022 Vojtech Aschenbrenner <v@asch.cz> */

#include <linux/kernel.h>
#include <linux/module.h>

#include "buse-blkdev.h"
#include "buse-chrdev.h"
#include "buse-configfs.h"
#include "buse-rqueue.h"
#include "buse-wqueue.h"
#include "main.h"

const char *buse_blkdev_name = "buse";
const int buse_blkdev_max_minors = 16;
int buse_blkdev_major;
struct class *buse_chrdev_class;

/*
 * Add new buse device with index and sets default parameters. All parameters can be changed via configfs.
 */
struct buse *buse_add(uint index)
{
	int ret;

	printk(KERN_DEBUG "buse: add new device\n");

	struct buse *buse = kzalloc(sizeof(*buse), GFP_KERNEL);
	if (!buse) {
		ret = -ENOMEM;
		printk(KERN_ERR "buse: error kzalloc\n");
		goto err;
	}

	atomic_set(&buse->stopped, 1);

	mutex_init(&buse->configfs_mutex);
	buse->index = index;
	buse->size = SZ_1G;
	buse->block_size = 512;
	buse->io_min = buse->block_size;
	buse->io_opt = buse->block_size;
	buse->write_chunk_size = 2 * SZ_1M;
	buse->write_shm_size = 32 * SZ_1M;
	buse->read_shm_size = buse->write_shm_size;
	buse->queue_depth = 64;
	buse->no_scheduler = true;
	buse->can_secure_erase = false;
	buse->can_discard = false;
	buse->can_write_same = false;
	buse->can_write_zeroes = false;
	buse->hw_queues = 1;
	buse->collision_area_size = 4096;

	return buse;

err:
	return ERR_PTR(ret);
}

/*
 * Checks whether all queues are connected and creates the block device eventually.
 */
void buse_blkdev_init_cond(struct buse *buse)
{
	int ret;

	if (!buse_wqueues_bound(buse) ||
			!buse_rqueues_bound(buse) ||
			buse->blkdev.created) {
		printk(KERN_DEBUG "buse: not all queues are bound. not registering disk");
		return;
	}
		

	buse->blkdev.created = true;
	buse_gendisk_register(buse);
	return;

	printk(KERN_DEBUG "buse: all queues is bound. creating block device");
	ret = buse_blkdev_init(buse);
	if (ret)
		goto err;

	return;

err:
	return;
}

/*
 * Initialize all structures for created device.
 */
int buse_on(struct buse *buse)
{
	int ret;

	printk(KERN_DEBUG "buse: buse on\n");

	buse->queues = kcalloc(buse->hw_queues, sizeof(*buse->queues), GFP_KERNEL);
	if (!buse->queues) {
		printk(KERN_ERR "buse: kcalloc failed\n");
		ret = -ENOMEM;
		goto err;
	}

	ret = buse_blkdev_init(buse);
	if (ret) {
		printk(KERN_ERR "buse: error initing block device, code %d\n", ret);
		goto err_queues;
	}

	printk(KERN_DEBUG "buse: succesfully inited block device\n");

	ret = buse_chrdev_init(buse);
	if (ret) {
		printk(KERN_ERR "buse: error initing character device, code %d\n", ret);
		goto err_blk;
	}

	printk(KERN_DEBUG "buse: succesfully inited char devices\n");

	ret = buse_rqueues_init(buse);
	if (ret) {
		printk(KERN_ERR "buse: error initing read queues, code %d\n", ret);
		goto err_chr;
	}

	printk(KERN_DEBUG "buse: succesfully inited read queues\n");

	ret = buse_wqueues_init(buse);
	if (ret) {
		printk(KERN_ERR "buse: error initing write queues, code %d\n", ret);
		goto err_r_init;
	}

	printk(KERN_DEBUG "buse: succesfully inited write queues\n");

	return 0;

err_r_init:
	buse_rqueues_exit(buse);
err_chr:
	buse_chrdev_exit(buse);
err_blk:
	buse_blkdev_exit(buse);
err_queues:
	kfree(buse->queues);
	buse->queues = NULL;
err:
	return ret;
}

/*
 * Deletes all the structures needed by the device.
 */
int buse_off(struct buse *buse)
{
	if (!buse->queues)
		return -EINVAL;

	if (buse_wqueues_bound(buse) ||
			buse_rqueues_bound(buse))
		return -EBUSY;

	buse_wqueues_exit(buse);
	buse_rqueues_exit(buse);
	buse_chrdev_exit(buse);
	buse_blkdev_exit(buse);
	kfree(buse->queues);
	buse->queues = NULL;

	return 0;
}

/*
 * Frees the buse structure.
 */
void buse_del(struct buse *buse)
{
	kfree(buse);
}

/*
 * Sends the termination chunks to all queues signaling that the device is stopping. This is
 * reaction to writing 0 to the power configfs attribute. When the userspace disconnect all the
 * queues, it can call buse_off().
 */
void buse_stop(struct buse *buse)
{
	int i;
	struct buse_wqueue *wq;
	struct buse_rqueue *rq;

	if (!buse->queues)
		return;

	for (i = 0; i < buse->num_queues; i++) {
		wq = &buse->queues[i].w;
		wqueue_send_term(wq);
	}

	for (i = 0; i < buse->num_queues; i++) {
		rq = &buse->queues[i].r;
		rqueue_send_term(rq);
	}
}

/*
 * Kernel module init function which is ran when the module is loaded. It just registers majors for
 * block device and character devices and initialize configfs subsystem. All further operations are
 * triggered from configfs.
 */
static int __init buse_init(void)
{
	int ret;

	buse_blkdev_major = register_blkdev(0, buse_blkdev_name);
	if (buse_blkdev_major < 0) {
		ret = buse_blkdev_major;
		goto err;
	}

	buse_chrdev_class = class_create(THIS_MODULE, buse_blkdev_name);
	if (IS_ERR(buse_chrdev_class)) {
		ret = PTR_ERR(buse_chrdev_class);
		goto err_blk;
	}

	ret = buse_configfs_init();
	if (ret)
		goto err_class;

	printk(KERN_DEBUG "buse: init success\n");
	return 0;

err_class:
	class_destroy(buse_chrdev_class);
err_blk:
	unregister_blkdev(buse_blkdev_major, buse_blkdev_name);
err:
	return ret;
}

/*
 * Kernel module exit function. Cleanup all module related structures. Module can be unloaded only
 * if all devices are destroyed.
 */
static void __exit buse_exit(void)
{
	class_destroy(buse_chrdev_class);
	unregister_blkdev(buse_blkdev_major, buse_blkdev_name);

	buse_configfs_exit();
}

module_init(buse_init);
module_exit(buse_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Vojtech Aschenbrenner <v@asch.cz>");
MODULE_DESCRIPTION("BUSE");
MODULE_VERSION("0.0.1");
