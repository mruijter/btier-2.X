/*
 * Btier bio request handling related funtions, block layer will call btier
 * make_request to handle block read and write requests.
 * Copyright (C) 2014 Mark Ruijter, <mruijter@gmail.com>
 *
 * Btier2 changes, Copyright (C) 2014 Jianjian Huo, <samuel.huo@gmail.com>
 * Get_chunksize function is from bcache.
 *
 */

#include "btier.h"

struct kmem_cache *bio_task_cache;

static void tier_submit_bio(struct tier_device *dev,
				unsigned int device,
				struct bio *bio,
				sector_t start_sector)
{
	struct block_device *bdev = dev->backdev[device]->bdev;

	set_debug_info(dev, BIO);

	bio->bi_iter.bi_sector	= start_sector;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0)
	bio_set_dev(bio, bdev);
#else
	bio->bi_bdev = bdev;
#endif

	generic_make_request(bio);
	clear_debug_info(dev, BIO);
}

unsigned int get_chunksize(struct block_device *bdev,
				  struct bio *bio)
{
        struct request_queue *q = bdev_get_queue(bdev);
        unsigned int chunksize;
        unsigned int max_hwsectors;
        unsigned int max_sectors;
	unsigned ret = 0, seg = 0;
	struct bio_vec bv;
	struct bvec_iter iter;

        max_hwsectors = queue_max_hw_sectors(q);
        max_sectors = queue_max_sectors(q);
        chunksize = min(max_hwsectors, max_sectors) << 9;

	bio_for_each_segment(bv, bio, iter) {
		if (seg == min_t(unsigned, BIO_MAX_PAGES,
				 queue_max_segments(q)))
			break;
		seg++;
		ret += bv.bv_len;
	}

	chunksize =  min(ret, chunksize);
	WARN_ON(!chunksize);
	/* chunksize should be aligned with sectors */
	WARN_ON(chunksize&((1 << 9) - 1));

	ret = max_t(int, chunksize, bio_iovec(bio).bv_len);

        if (chunksize > BLKSIZE)
            chunksize = BLKSIZE;

        return chunksize;
}

static int tier_moving_io(struct tier_device *dev,
			  struct blockinfo *binfo,
			  int rw)
{
	struct block_device *bdev = dev->backdev[binfo->device - 1]->bdev;
	struct bio *bio = dev->moving_bio;
	unsigned int done = 0;
	unsigned int cur_chunk = 0;
	struct bio *split;

        sector_t start;
	int res = 0;

	if (!bdev)
		return -EPERM;

	bio_reset(bio);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0)
	bio_set_dev(bio, bdev);
        bio->bi_opf = rw;
#else
	bio->bi_bdev = bdev;
        bio->bi_rw = rw;
#endif
	bio->bi_vcnt = BLKSIZE >> PAGE_SHIFT;
	bio->bi_iter.bi_sector = binfo->offset >> 9;
	bio->bi_iter.bi_size = BLKSIZE;
	bio->bi_iter.bi_idx = 0;
        bio->bi_iter.bi_bvec_done = 0;

	do {
		cur_chunk = get_chunksize(bdev, bio);
		if (cur_chunk > (BLKSIZE - done))
			cur_chunk = BLKSIZE - done;

		/* if no splits, and whole block is in one bio */
		if (1 == atomic_read(&bio->__bi_remaining) &&
		    cur_chunk == BLKSIZE) {
			set_debug_info(dev, BIO);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,8,0)
			res = submit_bio_wait(bio);
#else
			res = submit_bio_wait(rw, bio);
#endif
			clear_debug_info(dev, BIO);
			return res;
		}

		start = 0;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,18,0)
		split = bio_next_split(bio, cur_chunk >> 9,
				       GFP_NOIO, &fs_bio_set);
#else
                split = bio_next_split(bio, cur_chunk >> 9,
                                       GFP_NOIO, fs_bio_set);
#endif
		if (split == bio) {
			set_debug_info(dev, BIO);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,8,0)
			res = submit_bio_wait(bio);
#else
			res = submit_bio_wait(rw, bio);
#endif
			clear_debug_info(dev, BIO);
			return res;
		} else {
			bio_chain(split, bio);
			start = (binfo->offset + done) >> 9;
			tier_submit_bio(dev, binfo->device - 1, split, start);
		}

		done += cur_chunk;
	} while (done != BLKSIZE);

	WARN_ON(1);
	return -EPERM;
}

int tier_moving_block(struct tier_device *dev,
		      struct blockinfo *olddevice,
		      struct blockinfo *newdevice)
{
	int res;

	/* read the block from olddevice to moving_bio */
	res = tier_moving_io(dev, olddevice, READ);
	if (res) {
		tiererror(dev, "tier_moving_block : read failed\n");
		return -EIO;
	}

	/* write the block from moving_bio to newdevice */
	res = tier_moving_io(dev, newdevice, WRITE);
	if (res) {
		tiererror(dev, "tier_moving_block : write failed\n");
		return -EIO;
	}

	return 0;
}

static inline void increase_iostats(struct bio_task *bt)
{
	struct tier_device *dev = bt->dev;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,3,0)
        int rw = bio_data_dir(bt->parent_bio);
#else
	int rw = bio_rw(bt->parent_bio);
#endif

	if (rw) {
		if (bt->iotype == RANDOM)
			atomic64_inc(&dev->stats.rand_writes);
		else
			atomic64_inc(&dev->stats.seq_writes);
	} else {
		if (bt->iotype == RANDOM)
			atomic64_inc(&dev->stats.rand_reads);
		else
			atomic64_inc(&dev->stats.seq_reads);
	}
}

static inline void determine_iotype(struct bio_task *bt, u64 blocknr)
{
	int ioswitch = 0;
	struct tier_device *dev = bt->dev;

	spin_lock(&dev->io_seq_lock);

	if (blocknr >= dev->lastblocknr && blocknr <= dev->lastblocknr + 1) {
		ioswitch = 1;
	}

	if (ioswitch && dev->insequence < 10)
		dev->insequence++;
	else {
		if (dev->insequence > 0)
			dev->insequence--;
	}

	if (dev->insequence > 5) {
		bt->iotype = SEQUENTIAL;
	} else {
		bt->iotype = RANDOM;
	}

	dev->lastblocknr = blocknr;

	spin_unlock(&dev->io_seq_lock);
}

/* from bio->bi_iter.bi_sector, memset size of it to 0;
   size is guaranteed to be <= bi_size */
static void bio_fill_zero(struct bio *bio, unsigned int size)
{
	unsigned long flags;
	struct bio_vec bv;
	struct bvec_iter iter;
	unsigned int done = 0;

	bio_for_each_segment(bv, bio, iter) {
		char *data = bvec_kmap_irq(&bv, &flags);
		memset(data, 0, bv.bv_len);
		flush_dcache_page(bv.bv_page);
		bvec_kunmap_irq(data, &flags);

		done += bv.bv_len;
		if (done >= size)
			break;
	}
}

/* Check for corruption */
static int binfo_sanity(struct tier_device *dev, struct blockinfo *binfo)
{
	struct backing_device *backdev = dev->backdev[binfo->device - 1];

	if (binfo->device > dev->attached_devices) {
		pr_info
		    ("Metadata corruption detected : device %u, dev->attached_devices %u\n",
		     binfo->device, dev->attached_devices);
		tiererror(dev,
			  "get_blockinfo : binfo->device > dev->attached_devices");
		return 0;
	}

	if (binfo->offset > backdev->devicesize) {
		pr_info
		    ("Metadata corruption detected : device %u, offset %llu, devsize %llu\n",
		     binfo->device, binfo->offset, backdev->devicesize);
		tiererror(dev, "get_blockinfo : offset exceeds device size");
		return 0;
	}
	return 1;
}

/*
 * Read the metadata of the blocknr specified.
 * When a blocknr is not yet allocated binfo->device is 0; otherwhise > 0.
 * Metadata statistics are updated when called with
 * TIERREAD or TIERWRITE (updatemeta != 0 )
 */
struct blockinfo *get_blockinfo(struct tier_device *dev, u64 blocknr,
				int updatemeta)
{
	/* The blocklist starts at the end of the bitlist on device1 */
	struct blockinfo *binfo;
	struct backing_device *backdev = dev->backdev[0];

	if (dev->inerror)
		return NULL;

	binfo = backdev->blocklist[blocknr];

	if (0 != binfo->device) {
		if (!binfo_sanity(dev, binfo)) {
			binfo = NULL;
			goto err_ret;
		}
		backdev = dev->backdev[binfo->device - 1];

		/* update accesstime and hitcount */
		if (updatemeta > 0) {
			if (updatemeta == TIERREAD) {
				if (binfo->readcount < MAX_STAT_COUNT) {
					binfo->readcount++;
					spin_lock(&backdev->magic_lock);
					backdev->devmagic->total_reads++;
					spin_unlock(&backdev->magic_lock);
				}
			} else {
				if (binfo->writecount < MAX_STAT_COUNT) {
					binfo->writecount++;
					spin_lock(&backdev->magic_lock);
					backdev->devmagic->total_writes++;
					spin_unlock(&backdev->magic_lock);
				}
			}

			binfo->lastused = get_seconds();
		}
	}

err_ret:
	return binfo;
}

static int allocate_block(struct tier_device *dev, u64 blocknr,
			  struct blockinfo *binfo,
			  struct bio_task *bt)
{
	int device = 0;
	int count = 0;
	struct backing_device *backdev = dev->backdev[0];

	/* Sequential writes will go to SAS or SATA */
	if (bt->iotype == SEQUENTIAL && dev->attached_devices > 1) {
		spin_lock(&backdev->magic_lock);
		device =
		    backdev->devmagic->dtapolicy.sequential_landing;
		spin_unlock(&backdev->magic_lock);
	}

	while (1) {
		if (0 != allocate_dev(dev, blocknr, binfo, device))
			return -EIO;
		if (0 != binfo->device) {
			if (0 != write_blocklist(dev, blocknr, binfo, WA))
				return -EIO;
			break;
		}
		device++;
		count++;
		if (count >= dev->attached_devices) {
			pr_err
			    ("no free space found, this should never happen!!\n");
			return -ENOSPC;
		}
		if (device >= dev->attached_devices)
			device = 0;
	}
	return 0;
}

/* Reset blockinfo for this block to unused and clear the
   bitlist for this block
*/
void tier_discard(struct tier_device *dev, u64 offset, unsigned int size)
{
	struct blockinfo *binfo;
	u64 blocknr;
	u64 lastblocknr;
	u64 curoff;
	u64 start;

	pr_debug("Got a discard request offset %llu len %u\n",
		 offset, size);

	if (!dev->discard)
		return;
	curoff = offset + size;
	lastblocknr = curoff >> BLK_SHIFT;
	start = offset >> BLK_SHIFT;
	/* Make sure we don't discard a block while a part of it is still inuse */
	if ((start << BLK_SHIFT) < offset)
		start++;
	if ((start << BLK_SHIFT) > (offset + size))
		return;

	for (blocknr = start; blocknr < lastblocknr; blocknr++) {
		mutex_lock(dev->block_lock + blocknr);
		binfo = get_blockinfo(dev, blocknr, 0);
		if (dev->inerror) {
			mutex_unlock(dev->block_lock + blocknr);
			break;
		}
		if (binfo->device != 0) {
			pr_debug
			    ("really discard blocknr %llu at offset %llu size %u\n",
			     blocknr, offset, size);
			clear_dev_list(dev, binfo);
			reset_counters_on_migration(dev, binfo);
			discard_on_real_device(dev, binfo);
			memset(binfo, 0, sizeof(struct blockinfo));
			write_blocklist(dev, blocknr, binfo, WD);
		}
		mutex_unlock(dev->block_lock + blocknr);

		/* in case it's a huge discard */
		cond_resched();
	}
}

/*
 * Btier meta data operations, such as FLUSH/FUA, discard, and read/write
 * blocklist and bit list on backing devices.
 * Pending make_request will be waiting for those to be finished.
 * Cannot call them under generic_make_request, use a work queue.
 */
static void tier_meta_work(struct work_struct *work)
{
	struct bio_meta *bm = container_of(work, struct bio_meta, work);
	struct tier_device *dev = bm->dev;
	struct bio *bio = &bm->bio;
	struct bio *parent_bio = bm->parent_bio;
	int i, ret = 0;

	/*
	 * if bm->flush is true, flush possible dirty meta data if has.
	 * Currently, we don't have, other than r/w counts in block info.
	 */
	if (bm->flush) {
		/* send this zero size bio to every backing device*/
		set_debug_info(dev, PRESYNC);
		for (i = 0; i < dev->attached_devices; i++) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,10,0)
			bio_init(bio, NULL, 0);
#else
			bio_init(bio);
#endif
#ifdef RHEL86
        __bio_clone_fast(bio, parent_bio, GFP_NOIO);
#else
        __bio_clone_fast(bio, parent_bio);
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0)
			bio_set_dev(bio, dev->backdev[i]->bdev);
#else
			bio->bi_bdev = dev->backdev[i]->bdev;
#endif
			/* no need to set bi_end_io and bi_private */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,8,0)
			ret |= submit_bio_wait(bio);
#else
			ret |= submit_bio_wait(bio->bi_rw, bio);
#endif
		}
		clear_debug_info(dev, PRESYNC);
	}

	if (bm->discard) {
		set_debug_info(dev, DISCARD);
		tier_discard(dev, parent_bio->bi_iter.bi_sector << 9,
			     parent_bio->bi_iter.bi_size);
		clear_debug_info(dev, DISCARD);
	}

	if (bm->allocate) {
		set_debug_info(dev, PREALLOCBLOCK);
		ret = allocate_block(dev, bm->blocknr, bm->binfo, bm->bt);
		clear_debug_info(dev, PREALLOCBLOCK);
		if (ret != 0)
			pr_crit("Failed to allocate_block\n");
	}

	bm->ret = ret;
	complete(&bm->event);
}

static void tier_submit_and_wait_meta(struct bio_meta *bm)
{
	int ret = 0;
	struct tier_device *dev = bm->dev;

	init_completion(&bm->event);
	INIT_WORK(&bm->work, tier_meta_work);
	ret = queue_work(btier_wq, &bm->work);
	BUG_ON(!ret);

	/* wait until all those bio meta works have been finished*/
	wait_for_completion(&bm->event);

	if(bm->discard || bm->flush) {
		bio_endio(bm->parent_bio);

		atomic_dec(&dev->aio_pending);
		wake_up(&dev->aio_event);
	}

	if(bm->allocate && bm->ret) {
		/*
		 * couldn't allocate, error.
		 * need more error handling here.
		 */
		bm->binfo->device = 0;
	}

	mempool_free(bm, dev->bio_meta);
}

static inline void tier_dev_nodata(struct tier_device *dev,
				   struct bio *parent_bio)
{
	struct bio_meta *bm;

	bm = mempool_alloc(dev->bio_meta, GFP_NOIO);
	memset(bm, 0, sizeof(*bm));

	bm->dev = dev;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0)
	bm->flush = (parent_bio->bi_opf & (REQ_SYNC | REQ_FUA)) != 0;
#else
	bm->flush = (parent_bio->bi_rw & (REQ_FLUSH|REQ_FUA)) != 0;
#endif
	bm->parent_bio = parent_bio;
	tier_submit_and_wait_meta(bm);
}

static inline void tier_dev_discard(struct tier_device *dev,
				    struct bio *parent_bio)
{
	struct bio_meta *bm;

	bm = mempool_alloc(dev->bio_meta, GFP_NOIO);
	memset(bm, 0, sizeof(*bm));

	bm->dev = dev;
	bm->discard = 1;
	bm->parent_bio = parent_bio;

	tier_submit_and_wait_meta(bm);
}

static inline void tier_dev_allocate(struct tier_device *dev,
				    u64 blocknr,
				    struct blockinfo *binfo,
				    struct bio_task *bt)
{
	struct bio_meta *bm;

	bm = mempool_alloc(dev->bio_meta, GFP_NOIO);
	memset(bm, 0, sizeof(*bm));

	bm->dev = dev;
	bm->allocate = 1;
	bm->binfo = binfo;
	bm->blocknr = blocknr;
	bm->bt = bt;

	tier_submit_and_wait_meta(bm);
}

static void request_endio(struct bio *bio)
{
	struct bio_task *bt = bio->bi_private;
	struct tier_device *dev = bt->dev;

	bio_endio(bt->parent_bio);

	atomic_dec(&dev->aio_pending);
	wake_up(&dev->aio_event);
	mempool_free(bt, dev->bio_task);
}

static void tiered_dev_access(struct tier_device *dev, struct bio_task *bt)
{
	struct bio *bio = &bt->bio;
	u64 end_blk, cur_blk = 0, offset;
	struct blockinfo *binfo;
	unsigned int offset_in_blk, size_in_blk;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,3,0)
        int rw = bio_data_dir(bt->parent_bio);
#else
        int rw = bio_rw(bt->parent_bio);
#endif
        unsigned int done = 0;
        unsigned int cur_chunk = 0;
        sector_t start = 0;
        unsigned int device;
	struct bio *split;

	end_blk = ((bio_end_sector(bio) - 1) << 9) >> BLK_SHIFT;

	while (cur_blk <= end_blk) {
		offset = bio->bi_iter.bi_sector << 9;
		cur_blk = offset >> BLK_SHIFT;
		offset_in_blk = offset - (cur_blk << BLK_SHIFT);
		size_in_blk = (cur_blk == end_blk) ? bio->bi_iter.bi_size :
						     (BLKSIZE - offset_in_blk);

		determine_iotype(bt, cur_blk);
		increase_iostats(bt);

		mutex_lock(dev->block_lock + cur_blk);

		if (rw)
			binfo = get_blockinfo(dev, cur_blk, TIERWRITE);
		else
			binfo = get_blockinfo(dev, cur_blk, TIERREAD);

		/* read unallocated block, return data zero */
		if (unlikely(!rw && 0 == binfo->device)) {

			mutex_unlock(dev->block_lock + cur_blk);

			bio_fill_zero(bio, size_in_blk);

			bio_advance(bio, size_in_blk);

			/* total splits is 0 and it's now last blk of bio.*/
			if (1 == atomic_read(&bio->__bi_remaining) &&
			    cur_blk == end_blk) {
			        bio_endio(bt->parent_bio);
				goto bio_done;
			}

			/* total splits > 0 and it's now last blk of bio */
			if (atomic_read(&bio->__bi_remaining) > 1 &&
			    cur_blk == end_blk) {
				atomic_dec(&bio->__bi_remaining);
				goto bio_submitted_lastbio;
			}

			continue;
		}

		/* write unallocated space, allocate a new block */
		if (rw && 0 == binfo->device) {
			tier_dev_allocate(dev, cur_blk, binfo, bt);

			if(0 == binfo->device) {
				/*
				 * couldn't allocate, error.
				 * need more error handling here.
				 */
				bio_endio(bt->parent_bio);
				goto bio_done;
			}
		}

		/* access allocated block, split bio within it */
		done = 0;
		cur_chunk = 0;
		start = 0;
		device = binfo->device - 1;

		do {
			cur_chunk = get_chunksize(dev->backdev[device]->bdev,
						  bio);
			if (cur_chunk > (size_in_blk - done))
				cur_chunk = size_in_blk - done;

			/* if no splits, and it's now last blk of bio */
			if (1 == atomic_read(&bio->__bi_remaining) &&
			    cur_blk == end_blk &&
			    cur_chunk == size_in_blk) {
				start = (binfo->offset + offset_in_blk) >> 9;
				mutex_unlock(dev->block_lock + cur_blk);
				tier_submit_bio(dev, device, bio, start);
				goto bio_submitted_lastbio;
			}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,18,0)
			split = bio_next_split(bio, cur_chunk >> 9,
					       GFP_NOIO, &fs_bio_set);
#else
			split = bio_next_split(bio, cur_chunk >> 9,
					       GFP_NOIO, fs_bio_set);
#endif
			if (split == bio) {
				BUG_ON(cur_blk != end_blk);
				start = (binfo->offset + offset_in_blk + done)
					>> 9;
				mutex_unlock(dev->block_lock + cur_blk);
				tier_submit_bio(dev, device, bio, start);
				goto bio_submitted_lastbio;
			} else {
				bio_chain(split, bio);
				start = (binfo->offset + offset_in_blk + done)
					>> 9;
				tier_submit_bio(dev, device, split, start);
			}

			done += cur_chunk;
		} while (done != size_in_blk);

		/* splitting in current block is done, go to next block.*/
		mutex_unlock(dev->block_lock + cur_blk);
	}

	return;

bio_done:
	mempool_free(bt, dev->bio_task);
	atomic_dec(&dev->aio_pending);
	wake_up(&dev->aio_event);
bio_submitted_lastbio:
	return;
}

static inline struct bio_task *task_alloc(struct tier_device *dev,
					  struct bio *parent_bio)
{
	struct bio_task *bt;
	struct bio *bio;

	bt = mempool_alloc(dev->bio_task, GFP_NOIO);
	memset(bt, 0, sizeof(*bt));

	bt->parent_bio = parent_bio;
	bt->dev = dev;
	bt->iotype = RANDOM;

	bio = &bt->bio;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,10,0)
	bio_init(bio, NULL, 0);
#else
	bio_init(bio);
#endif
#ifdef RHEL86
	__bio_clone_fast(bio, parent_bio, GFP_NOIO);
#else
	__bio_clone_fast(bio, parent_bio);
#endif
	bio->bi_end_io  = request_endio;
	bio->bi_private = bt;

	return bt;
}

blk_qc_t tier_make_request(struct request_queue *q, struct bio *parent_bio)
{
	struct tier_device *dev = q->queuedata;
	struct bio_task *bt;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,3,0)
        int rw = bio_data_dir(parent_bio);
#else
        int rw = bio_rw(parent_bio);
#endif
#ifndef RHEL86
	int cpu;
#endif

	atomic_set(&dev->wqlock, NORMAL_IO);
	down_read(&dev->qlock);

	BUG_ON(!dev || (rw != READ && rw != WRITE));

	/* if deregister already happens, or very bad error happens */
	if (unlikely(!dev->active || dev->inerror))
		goto out;
#ifdef RHEL86
        part_stat_inc(&dev->gd->part0, ios[rw]);
        part_stat_add(&dev->gd->part0, sectors[rw], bio_sectors(parent_bio));
#else
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,17,0)
        cpu = part_stat_lock();
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,0,0)
        part_stat_inc(&dev->gd->part0, ios[rw]);
        part_stat_add(&dev->gd->part0, sectors[rw], bio_sectors(parent_bio));
#else
        part_stat_inc(cpu, &dev->gd->part0, ios[rw]);
        part_stat_add(cpu, &dev->gd->part0, sectors[rw], bio_sectors(parent_bio));
#endif
        part_stat_unlock();
#endif
#endif

	/* increase aio_pending for each bio */
	atomic_inc(&dev->aio_pending);

	if (unlikely(!parent_bio->bi_iter.bi_size)) {
		tier_dev_nodata(dev, parent_bio);
	} else {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,8,0)
                if (rw && (bio_op(parent_bio) == REQ_OP_DISCARD)) {
#else
                if (rw && (parent_bio->bi_rw & REQ_DISCARD)) {
#endif
			tier_dev_discard(dev, parent_bio);
			goto end_return;
		}

		bt = task_alloc(dev, parent_bio);

		tiered_dev_access(dev, bt);
	}

	goto end_return;

out:
	bio_io_error(parent_bio);
end_return:
	atomic_set(&dev->wqlock, 0);
	up_read(&dev->qlock);
	return BLK_QC_T_NONE;
}

void tier_request_exit(void)
{
	if (bio_task_cache)
		kmem_cache_destroy(bio_task_cache);
}

int __init tier_request_init(void)
{
	bio_task_cache = KMEM_CACHE(bio_task, 0);
	if (!bio_task_cache)
		return -ENOMEM;

	return 0;
}
