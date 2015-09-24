#include <linux/module.h>
#include <linux/io.h>
#include <linux/genhd.h>
#include <linux/hdreg.h>
#include <linux/moduleparam.h>
#include <linux/ide.h>
#include <linux/stat.h>
#include <linux/kfifo.h>
#include <linux/semaphore.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/sched/rt.h>
#include <linux/ktime.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/time.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/spinlock_types.h>

#include <asm/delay.h>

#include <linux/ftrace.h>



static long speed_factor = 1000;
module_param(speed_factor, long, S_IRUGO);

static long disk_channels = 1;
module_param(disk_channels, long, S_IRUGO);

static long extra_delay = 0; // artificial deplay time in micro seconds.
module_param(extra_delay, long, S_IRUGO);




int SSD_RESPONSE_TIME_READ=1000000;
int SSD_RESPONSE_TIME_WRITE=2000000;

static int request_size_kb[8] = {512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
static int iorate_table_random[2][8] = {{194672, 189513, 186798, 176876, 159182, 141700, 104546, 66149}, {898, 899, 898, 862, 778, 795, 450, 370}}; // unit: 1/100 iops
//static int iorate_table_random[2][8] = {{19467, 19467, 19467, 19467, 19467, 19467, 19467, 19467}, {19467, 19467, 19467, 19467, 19467, 19467, 19467, 19467}}; // unit: 1/100 iops
static int response_time_table_random[2][8];

static int iorate_table_seq[2][8] = {{522101, 488278, 429834, 371532, 298845, 281019, 146861, 71935}, {108649, 104858, 101045, 87071, 73645, 289488, 143416, 72945}}; // unit: 1/100 iops
//static int iorate_table_seq[2][8] = {{19467, 19467, 19467, 19467, 19467, 19467, 19467, 19467}, {19467, 19467, 19467, 19467, 19467, 19467, 19467, 19467}}; // unit: 1/100 iops


static int response_time_table_seq[2][8];


// The number of parallel requests that the emulated disk can handle. 
#define MAX_DISK_REQUESTS_IN_PARALLEL 8

#define SECTOR_SIZE 512
#define MAX_REQUEST_SECTOR_NUM 128

#define SPLIT_DATA_TRANSFER 1
#define DIFFERENT_SUBMIT_TIME

/*
NOTE: The example code in the ldd book is using out-dated block layer API.
Below is a updated verison for reference. BUT, it has some bugs...:
http://maratux.blogspot.tw/2011/04/updated-minibd-driver-for-linux-sbullc.html
*/


const unsigned long memory_storage_start = 0x07000000; // The start address of the system memory that can be used as backing store.
const unsigned long memory_storage_size = 0x19000000; // The size of the backing store. (512-112)=400MB

typedef struct
{
	long size;
	unsigned char *data;
	spinlock_t lock;
	struct request_queue *queue;
	struct gendisk *gd;
	int major;

	long request_inflight;

	struct task_struct *processing_task;

	bool stopping; // For letting the processing thread to know that we are unloading.
	
} nctuss_emudisk_t;

static nctuss_emudisk_t  Emudisk;

struct nctuss_emudisk_channel
{
	ktime_t request_completion_time_kt;
	bool request_completed;

	struct request *request;
	long request_rw;
	unsigned long request_last_sector;

};
struct nctuss_emudisk_channel nctuss_emudisk_channels[MAX_DISK_REQUESTS_IN_PARALLEL];

static void nctuss_emudisk_softirq_done(struct request *req)
{

	//printk(KERN_ALERT "nctuss_emudisk_softirq_done\n");
	//trace_printk("nctuss_emudisk_softirq_done\n");

	unsigned long flags;
	blk_end_request_all(req, 0);

	spin_lock_irqsave(&(Emudisk.lock), flags);
	
	Emudisk.request_inflight--;
	if(queue_flag_test_and_clear(QUEUE_FLAG_STOPPED, req->q)) {
		//printk(KERN_ALERT "nctuss_emudisk_softirq_done blk_start_queue\n");
		blk_start_queue(req->q); // Must be called with queue lock held.
	}

	spin_unlock_irqrestore(&(Emudisk.lock), flags);
}

int nctuss_emudisk_getgeo(struct block_device *block_device, struct hd_geometry *geo) {

	long size;

	/* We have no real geometry, of course, so make something up. */

	nctuss_emudisk_t *dev = block_device->bd_disk->private_data;
	size = dev->size * 1;
//	geo->cylinders = (size & ~0x3f) >> 6;
//	geo->heads = 4;
//	geo->sectors = 16;
	geo->cylinders = size / 63 / 255;
	geo->heads = 255;
	geo->sectors = 63;
	geo->start = 0;
	return 0;
}


static struct block_device_operations nctuss_emudisk_ops = {
	.getgeo = nctuss_emudisk_getgeo
};



/*
* Actual I/O processing.
*/
static void nctuss_emudisk_transfer(nctuss_emudisk_t *dev, struct request *req)
{
	unsigned long offset = blk_rq_pos(req) * SECTOR_SIZE;
	unsigned long nbytes = blk_rq_bytes(req);

	struct req_iterator iter;
	struct bio_vec *bvec;
	unsigned long flags;
	size_t size;
	unsigned char *buf;

	if(unlikely((offset+nbytes) > dev->size * SECTOR_SIZE)) {
		printk(KERN_ALERT "nctuss_ramdisk: working past the end of the block device. offset=%ld, nbytes=%ld.\n",
			offset, nbytes);
		while(1);
		return;
	}

	rq_for_each_segment(bvec, req, iter) {
		size = bvec->bv_len;
		buf = bvec_kmap_irq(bvec, &flags);

		if(rq_data_dir(req) == WRITE) {
			memcpy(Emudisk.data + offset, buf, size);
		}
		else {
			memcpy(buf, Emudisk.data + offset, size);
		}
		offset+=size;
		
		bvec_kunmap_irq(buf, &flags);
	}

	
}

static bool next_response_to_reply = false;
ktime_t next_response_to_reply_time_kt;

static int nctuss_emudisk_processing_task_func(void *dummy)
{
	ktime_t current_time_kt, sleep_until_time_kt;
	u64 next_response_time;
	int ret;
	int i, next_response_index;
	bool request_completed, try_process_next_without_sleep;


	spin_lock_bh(&(Emudisk.lock));
	
	while(1) {
		//printk(KERN_ALERT "nctuss_emudisk_processing_task_func - 1\n");
		
		if(next_response_to_reply == false) {
			set_current_state(TASK_INTERRUPTIBLE);
			spin_unlock_bh(&(Emudisk.lock));
			schedule_hrtimeout(NULL, HRTIMER_MODE_ABS); // sleep until interrupted
			spin_lock_bh(&(Emudisk.lock));
		}

		resleep:
		sleep_until_time_kt = next_response_to_reply_time_kt;
		set_current_state(TASK_INTERRUPTIBLE);
		spin_unlock_bh(&(Emudisk.lock));
		ret = schedule_hrtimeout(&sleep_until_time_kt, HRTIMER_MODE_ABS);

		if(unlikely(Emudisk.stopping == true)) {
			break;
		}
		spin_lock_bh(&(Emudisk.lock));

		if(unlikely(ret==-EINTR)) {
			//printkk(KERN_ALERT "nctuss_emudisk_processing_task_func - ret==-EINTR\n");
			goto resleep;
		}
	
		current_time_kt = ktime_get();

		if(unlikely(ktime_compare(next_response_to_reply_time_kt, current_time_kt) > 0)) {
			printk(KERN_ALERT "nctuss_ramdisk: next_response_to_reply_time_kt (%llu) > current_time_kt (%llu)\n",
				next_response_to_reply_time_kt.tv64, current_time_kt.tv64);
			while(1);
		}

		/* Process completed requests */
		request_completed = false;

		process_next_without_sleep:

#if 1 // Commit response to kernel in strickly completion time order.

		// find the next response with the earliest compeltion time.
		next_response_index = -1;
		for(i=0; i<disk_channels; i++) {
			if(nctuss_emudisk_channels[i].request_completed == false && (nctuss_emudisk_channels[i].request_completion_time_kt.tv64 < current_time_kt.tv64)) {
				if(next_response_index == -1) {
					next_response_index = i;
				}
				else if(nctuss_emudisk_channels[i].request_completion_time_kt.tv64 < nctuss_emudisk_channels[next_response_index].request_completion_time_kt.tv64) {
					next_response_index = i;
				}
			}
		}

		if(next_response_index!=-1) {
			/* Data transfer */
#ifdef SPLIT_DATA_TRANSFER				
			if(rq_data_dir(nctuss_emudisk_channels[next_response_index].request) == READ) {
				nctuss_emudisk_transfer(&Emudisk, nctuss_emudisk_channels[next_response_index].request);
			}
#else
			nctuss_emudisk_transfer(&Emudisk, nctuss_emudisk_channels[next_response_index].request);
#endif		
			request_completed = true;
			try_process_next_without_sleep = true;
			blk_complete_request(nctuss_emudisk_channels[next_response_index].request); 
			nctuss_emudisk_channels[next_response_index].request_completed = true;
		}

		if(unlikely(!request_completed)) {
			printk(KERN_ALERT "NOT request_completed. current_time=%llu, next_response_to_reply_time_kt=%llu\n",
				current_time_kt.tv64, next_response_to_reply_time_kt.tv64);
			for(i=0; i<disk_channels; i++) {
				printk(KERN_ALERT "[%d] request_completed = %d\n", i, nctuss_emudisk_channels[i].request_completed);
				printk(KERN_ALERT "[%d] request_completion_time_kt.tv64 = %llu\n", i, nctuss_emudisk_channels[i].request_completion_time_kt.tv64);				
				
			}			
		}

#else		

		for(i=0; i<disk_channels; i++) {
			if(nctuss_emudisk_channels[i].request_completed == false &&
					(nctuss_emudisk_channels[i].request_completion_time_kt.tv64 < current_time_kt.tv64)) {

				//spin_unlock_bh(&(Emudisk.lock));
				
				/* Data transfer */
#ifdef SPLIT_DATA_TRANSFER				
				if(rq_data_dir(nctuss_emudisk_channels[i].request) == READ) {
					nctuss_emudisk_transfer(&Emudisk, nctuss_emudisk_channels[i].request);
				}
#else
				nctuss_emudisk_transfer(&Emudisk, nctuss_emudisk_channels[i].request);
#endif

				//spin_lock_bh(&(Emudisk.lock));

				// submit request to kernel
				//printk(KERN_ALERT "nctuss_emudisk_processing_task_func - 4\n");

				request_completed = true;
				try_process_next_without_sleep = true;
				blk_complete_request(nctuss_emudisk_channels[i].request);	
				nctuss_emudisk_channels[i].request_completed = true;

				//printk(KERN_ALERT "nctuss_emudisk_processing_task_func - 5\n");
			}
		}
		if(unlikely(!request_completed)) {
			printk(KERN_ALERT "NOT request_completed. current_time=%llu, next_response_to_reply_time_kt=%llu\n",
				current_time_kt.tv64, next_response_to_reply_time_kt.tv64);
			for(i=0; i<disk_channels; i++) {
				printk(KERN_ALERT "[%d] request_completed = %d\n", i, nctuss_emudisk_channels[i].request_completed);
				printk(KERN_ALERT "[%d] request_completion_time_kt.tv64 = %llu\n", i, nctuss_emudisk_channels[i].request_completion_time_kt.tv64);				
				
			}
		}
#endif

		if(try_process_next_without_sleep) {
			current_time_kt = ktime_get();
			try_process_next_without_sleep = false;
			goto process_next_without_sleep;
		}

		//printk(KERN_ALERT "nctuss_emudisk_processing_task_func - 6\n");

		/* Find next request to complete */
		next_response_time = ULLONG_MAX;
		for(i=0; i<disk_channels; i++) {
			if(nctuss_emudisk_channels[i].request_completed == false &&
				(nctuss_emudisk_channels[i].request_completion_time_kt.tv64 < next_response_time)) {
				next_response_time = nctuss_emudisk_channels[i].request_completion_time_kt.tv64;
			}
		}
		if(next_response_time != ULLONG_MAX) {
			next_response_to_reply_time_kt.tv64 = next_response_time;
			next_response_to_reply = true;
		}
		else {
			next_response_to_reply = false;
		}

	}
	
	return 0;
}
	
static void nctuss_emudisk_strategy(struct request_queue *q)
{
	//printk(KERN_ALERT "nctuss_emudisk_strategy\n");
	// NOTE: Emudisk.lock (which should be q->queue_lock?) is accquired by kernel when entering this function.
	
	struct request *req;
	ktime_t current_time_kt, completion_time_kt;
	u64 most_current_response_time;
	bool need_to_wake_up_reply_thread = false;
	int i;
	bool is_sequential_request;
	int disk_channel_to_be_used;
	int request_size_index;
	int nbytes;

#ifndef DIFFERENT_SUBMIT_TIME
	current_time_kt = ktime_get();
#endif
	
	while(true) {
#ifdef DIFFERENT_SUBMIT_TIME
			current_time_kt = ktime_get();
#endif

		//printk(KERN_ALERT "nctuss_emudisk_strategy - 1\n");
		req = blk_fetch_request(q);
		if(req==NULL) {
			//printk(KERN_ALERT "nctuss_emudisk_strategy - !2\n");
			break;
		}

		if(unlikely(req->cmd_type != REQ_TYPE_FS)) {
			printk (KERN_ALERT "nctuss_ramdisk:nctuss_emudisk_strategy: Skip non-CMD request\n");
			blk_end_request_all(req, -EIO);
			continue;
		}

		/* Calculate response time */

		// Check if request is sequential read/write
		is_sequential_request = false;
		for(i=0; i<disk_channels; i++) {
			if(nctuss_emudisk_channels[i].request_rw == rq_data_dir(req) && nctuss_emudisk_channels[i].request_last_sector == blk_rq_pos(req)) {
				is_sequential_request = true;
			}
		}

		nbytes = blk_rq_bytes(req);
		request_size_index = -1;
		for(i=0; i<8; i++) {
			if(nbytes <= request_size_kb[i]) {
				request_size_index = i;
				break;
			}
		}
		if(unlikely(request_size_index==-1)) {
			printk(KERN_ALERT "nctuss_ramdisk: nctuss_emudisk_strategy: request_size_index==-1\n");
			while(1);			
		}
		

		if(is_sequential_request == true) {
			if(rq_data_dir(req) == WRITE) {
				completion_time_kt.tv64 =  current_time_kt.tv64 + response_time_table_seq[WRITE][request_size_index];
			}
			else {
				completion_time_kt.tv64 =  current_time_kt.tv64 + response_time_table_seq[READ][request_size_index];
			}			
		}
		else {
			if(rq_data_dir(req) == WRITE) {
				completion_time_kt.tv64 =  current_time_kt.tv64 + response_time_table_random[WRITE][request_size_index];
			}
			else {
				completion_time_kt.tv64 =  current_time_kt.tv64 + response_time_table_random[READ][request_size_index];
			}			
		}

		/* Artificial extra delay */
		if(extra_delay!=0) {
			udelay(extra_delay);
		}

	
		// Try to find an empty slot in emudisk
		// Should find the oldeset completed disk channel to match the behavior with emulator server.
		// The channel to be used could affect the sequential/random decision of future I/O requests.
		disk_channel_to_be_used = -1;
		for(i=0; i<disk_channels; i++) {
			if(nctuss_emudisk_channels[i].request_completed == true) {
				if(disk_channel_to_be_used == -1) {
					disk_channel_to_be_used = i;
				}
				else if(nctuss_emudisk_channels[disk_channel_to_be_used].request_completion_time_kt.tv64 > nctuss_emudisk_channels[i].request_completion_time_kt.tv64) {
					disk_channel_to_be_used = i;
				}
			}
		}

		if(likely(disk_channel_to_be_used != -1)) {
			nctuss_emudisk_channels[disk_channel_to_be_used].request_completion_time_kt = completion_time_kt;
			nctuss_emudisk_channels[disk_channel_to_be_used].request = req;
			nctuss_emudisk_channels[disk_channel_to_be_used].request_completed = false;
			nctuss_emudisk_channels[disk_channel_to_be_used].request_rw = rq_data_dir(req);
			nctuss_emudisk_channels[disk_channel_to_be_used].request_last_sector = blk_rq_pos(req) + (blk_rq_bytes(req)+(SECTOR_SIZE-1))/SECTOR_SIZE;
#ifdef SPLIT_DATA_TRANSFER				
			if(rq_data_dir(nctuss_emudisk_channels[disk_channel_to_be_used].request) == WRITE) {
				nctuss_emudisk_transfer(&Emudisk, nctuss_emudisk_channels[disk_channel_to_be_used].request);
			}
#endif
		}

		if(unlikely(disk_channel_to_be_used==-1)) {
			printk(KERN_ALERT "nctuss_ramdisk: cannot find empty slot in emudisk. request_inflight=%ld\n", Emudisk.request_inflight);
			while(1);
		}
		
		// Find the most recent response
		most_current_response_time = completion_time_kt.tv64;
		for(i=0; i<disk_channels; i++) {
			if(nctuss_emudisk_channels[i].request_completed == false &&
				(nctuss_emudisk_channels[i].request_completion_time_kt.tv64 < most_current_response_time)) {
				most_current_response_time = nctuss_emudisk_channels[i].request_completion_time_kt.tv64;
			}
		}
		// ... wake processing process
		if(next_response_to_reply == false || (most_current_response_time < next_response_to_reply_time_kt.tv64)) {
			next_response_to_reply_time_kt.tv64 = most_current_response_time;
			next_response_to_reply = true;

			need_to_wake_up_reply_thread = true;
		}
	
		Emudisk.request_inflight++;

/*
		if(Emudisk.request_inflight>=2) {
			printk(KERN_ALERT "Emudisk.request_inflight = %ld\n", Emudisk.request_inflight);
		}
*/
		if(Emudisk.request_inflight >= disk_channels) {
			//printk(KERN_ALERT "nctuss_emudisk_strategy - !\n");
			blk_stop_queue(q);
			break;
		}
	}
	//printk(KERN_ALERT "nctuss_emudisk_strategy - 2\n");

	if(need_to_wake_up_reply_thread==true) {
		if(unlikely(Emudisk.processing_task->state != TASK_INTERRUPTIBLE)) {
			////printk(KERN_ALERT "nctuss_emudisk: Emudisk.processing_task not sleeping...state=%ld\n", Emudisk.processing_task->state);
		}
		//printk(KERN_ALERT "nctuss_emudisk_strategy - 2.5\n");
		if(wake_up_process(Emudisk.processing_task)==0) {
			////printk(KERN_ALERT "nctuss_emudisk: Emudisk.processing_task not sleeping...state=%ld\n", Emudisk.processing_task->state);
		}
	}
	//printk(KERN_ALERT "nctuss_emudisk_strategy - 3\n");
	//printk(KERN_ALERT "nctuss_emudisk_strategy exit\n");
}

static void setup_nctuss_emudisk_processing_task(void)
{
	int ret;
	static const struct sched_param param = {
		.sched_priority = MAX_USER_RT_PRIO/2,
		//.sched_priority = 0, // 0 is the highest priority
	};

	Emudisk.processing_task = kthread_create(nctuss_emudisk_processing_task_func, NULL, "nctuss_emudisk_processing");
	if(IS_ERR(Emudisk.processing_task)) {
		printk(KERN_ALERT "nctuss_ramdisk: processing task creation failed.\n");
		while(1) {};
	}


	ret = sched_setscheduler(Emudisk.processing_task, SCHED_FIFO, &param);
	if(ret < 0) {
		printk(KERN_ALERT "nctuss_ramdisk: processing task sched_setscheduler failed.\n");
		while(1) {};		
	}

		
	wake_up_process(Emudisk.processing_task);

}

static int __init emudisk_init(void)
{
	int dev_major;
	unsigned char *data;
	int i, j;

	if(disk_channels > MAX_DISK_REQUESTS_IN_PARALLEL) {
		printk(KERN_ALERT "nctuss_ramdisk: disk_channels > %d\n", MAX_DISK_REQUESTS_IN_PARALLEL);
		goto err_channel_num_not_supported;
	}

	//printk(KERN_ALERT "emudisk_init: speed_factor = %ld\n", speed_factor);

	//SSD_RESPONSE_TIME_READ = 1000000 / 1000 * speed_factor;
	//SSD_RESPONSE_TIME_WRITE = 2000000 / 1000 * speed_factor;

	for(i=0; i<disk_channels ;i++) {
		nctuss_emudisk_channels[i].request_completion_time_kt.tv64 = 0;
		nctuss_emudisk_channels[i].request_completed = true;
	}
	
	memset(&Emudisk, 0, sizeof(nctuss_emudisk_t));
	
	dev_major = register_blkdev(0, "nctuss_emudisk");

	if(dev_major <=0) {
		printk(KERN_WARNING "nctuss_ramdisk: unable to get major number");
		return -EBUSY;
	}

	/* */
	for(j=0; j<2; j++) {
		for(i=0; i<8; i++) {
			response_time_table_random[j][i] = ((1000000000 / iorate_table_random[j][i]) * 100) / 1000 * speed_factor;
			response_time_table_seq[j][i] = ((1000000000 / iorate_table_seq[j][i]) * 100) / 1000 * speed_factor;

			//response_time_table_random[j][i] = 0;
			//response_time_table_seq[j][i] = 0;
		}
	}
	

	/* Initialize Emudisk */

	Emudisk.size = 0x19000000/SECTOR_SIZE; // 400MB
	//Emudisk.size = 0x10000000/SECTOR_SIZE; // 256MB
	printk(KERN_ALERT "nctuss_ramdisk: disk_size(sectors) = %ld\n", Emudisk.size);	

	spin_lock_init(&Emudisk.lock);

	Emudisk.stopping = false;


	/* Create request queue */
	Emudisk.queue = blk_init_queue(nctuss_emudisk_strategy, &(Emudisk.lock));
	if(Emudisk.queue == NULL) {
		printk(KERN_WARNING "nctuss_ramdisk: error on blk_init_queue\n");
		goto err_init_queue;
	}
	blk_queue_max_hw_sectors(Emudisk.queue, MAX_REQUEST_SECTOR_NUM);
	blk_queue_max_segments(Emudisk.queue, MAX_REQUEST_SECTOR_NUM);
	queue_flag_set_unlocked(QUEUE_FLAG_NONROT, Emudisk.queue); 


	Emudisk.gd = alloc_disk(16);
	if(!Emudisk.gd) {
		printk(KERN_WARNING "nctuss_ramdisk: alloc_disk() failed");
		goto err_no_gendisk;
	}

	/* Ioremap */
	// HINT: use memmap=nnG$ssG at in boot parameter to reserver memory.
	//ON T61: 0x300000000\$0x100000000


	data = (unsigned char *)ioremap_nocache(memory_storage_start, memory_storage_size);
	if(data==NULL) {
		goto err_ioremap;
	} else {
		Emudisk.data = data;
	}


	Emudisk.gd->queue = Emudisk.queue;
	Emudisk.gd->private_data = &Emudisk;
	Emudisk.major = Emudisk.gd->major = dev_major;
	Emudisk.gd->first_minor = 0;
	set_capacity(Emudisk.gd, Emudisk.size);
	snprintf(Emudisk.gd->disk_name, DISK_NAME_LEN, "emudisk0");
	Emudisk.gd->fops = &nctuss_emudisk_ops;

	blk_queue_softirq_done(Emudisk.queue, nctuss_emudisk_softirq_done);

	setup_nctuss_emudisk_processing_task();

	add_disk(Emudisk.gd);

	return 0;


	err_ioremap:
	err_no_gendisk:
		blk_cleanup_queue(Emudisk.queue);
	err_init_queue:
		unregister_blkdev(dev_major, "emudisk0");
		
		return -ENOMEM;
	err_channel_num_not_supported:
		return -EINVAL;

}

static void __exit emudisk_exit(void)
{

	
	del_gendisk(Emudisk.gd);
	put_disk(Emudisk.gd);

	blk_cleanup_queue(Emudisk.queue);
	unregister_blkdev(Emudisk.major, "nctuss_emudisk");

	Emudisk.stopping = true;

	kthread_stop(Emudisk.processing_task);

	iounmap(Emudisk.data);
	
    printk(KERN_ALERT "NCTUSS EmuDisk module unload\n");
}


module_init(emudisk_init);
module_exit(emudisk_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ming-Ju Wu");
MODULE_DESCRIPTION("NCTU Storage Simulator - Emu Disk Mod");

