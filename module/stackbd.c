#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/kernel.h> /* printk() */
#include <linux/fs.h>     /* everything... */
#include <linux/errno.h>  /* error codes */
#include <linux/types.h>  /* size_t */
#include <linux/vmalloc.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/hdreg.h>
#include <linux/kthread.h>


#include <trace/events/block.h>

#include "../common/stackbd.h"

#define STACKBD_BDEV_MODE (FMODE_READ | FMODE_WRITE | FMODE_EXCL)
#define DEBUGGG printk("stackbd: %d\n", __LINE__);
/*
 * We can tweak our hardware sector size, but the kernel talks to us
 * in terms of small sectors, always.
 */
#define KERNEL_SECTOR_SIZE 512
#define MY_TRACE_MODULE "block_trace"

#define LOG_ENTRIES_BUFFER_SIZE 100

MODULE_LICENSE("Dual BSD/GPL");

static int major_num = 0;
static int trace_dev_major = 0;
module_param(major_num, int, 0);
static int LOGICAL_BLOCK_SIZE = 512;
module_param(LOGICAL_BLOCK_SIZE, int, 0);

// Structure for our log entries
typedef struct _log_t {
  unsigned int pid;
  int is_write; 
  loff_t offset;
  size_t count;
} log_t;

log_t* logs; // pointer to buffer of logs
unsigned int log_read_head, log_write_head;
unsigned int overruns; // number of times there was an entry that was overwritten
unsigned int logging_enabled;

static int log_entries_count(void) {
  int diff = log_write_head - log_read_head;
  if (diff < 0)
    diff += LOG_ENTRIES_BUFFER_SIZE;
  return diff;
}

static void log_increment_read_head(int count) {
  log_read_head = (log_read_head + count) % LOG_ENTRIES_BUFFER_SIZE;
}

static void log_increment_write_head(void) {
  log_write_head = (log_write_head + 1) % LOG_ENTRIES_BUFFER_SIZE;
  // Check if we rolled over the read head, i.e. overrun
  if (log_write_head == log_read_head) {
    log_increment_read_head(1);
    overruns++;
    printk(KERN_WARNING "log overrun. total overruns is %d", overruns);
  }
}

static void log_write(unsigned int pid, int write, long offset, unsigned int count) {
  log_t* log_entry = logs + log_write_head;

  *log_entry = (log_t){
    .pid = pid,
    .is_write = write,
    .offset = offset,
    .count = count
  };
  log_increment_write_head();
}

static int logger_open(struct inode * in, struct file * fd) {
  logging_enabled++;
  printk(KERN_INFO "added reader, number is now %d", logging_enabled);
  return 0;
}

static char* _logger_read(char *buffer, size_t count) {
  copy_to_user(buffer, logs + log_read_head, count * sizeof(log_t));
  log_increment_read_head(count);
  return buffer + count*sizeof(log_t);
}

static ssize_t logger_read(struct file *fd, char *buffer, size_t count, loff_t *offset) {
  size_t first_read_size, second_read_size;
  size_t log_count = log_entries_count();
  if (count > log_count)
    count = log_count;
  first_read_size = count;
  second_read_size = 0;
  if (log_read_head + count > LOG_ENTRIES_BUFFER_SIZE) {
    first_read_size = LOG_ENTRIES_BUFFER_SIZE - log_read_head;
    second_read_size = count - first_read_size;
  }
  buffer = _logger_read(buffer, first_read_size);
  buffer = _logger_read(buffer, second_read_size);
  return count;
}

static int logger_release(struct inode *in, struct file *fd) {
  logging_enabled--;
  printk(KERN_INFO "removed reader, number is now %d", logging_enabled);
  return 0;
}

static struct file_operations my_trace_fops = {
  .read = logger_read,
  .open = logger_open,
  .release = logger_release
};



/*
 * The internal representation of our device.
 */
static struct stackbd_t {
    sector_t capacity; /* Sectors */
    struct gendisk *gd;
    spinlock_t lock;
    struct bio_list bio_list;    
    struct task_struct *thread;
    int is_active;
    struct block_device *bdev_raw;
    /* Our request queue */
    struct request_queue *queue;
} stackbd;

static DECLARE_WAIT_QUEUE_HEAD(req_event);

static void stackbd_io_fn(struct bio *bio)
{
//    printk("stackdb: Mapping sector: %llu -> %llu, dev: %s -> %s\n",
//            bio->bi_sector,
//            lba != EMPTY_REAL_LBA ? lba : bio->bi_sector,
//            bio->bi_bdev->bd_disk->disk_name,
//            bdev_raw->bd_disk->disk_name);
//
//    if (lba != EMPTY_REAL_LBA)
//        bio->bi_sector = lba;
    bio->bi_disk = stackbd.bdev_raw->bd_disk;

//    trace_block_bio_remap(bdev_get_queue(stackbd.bdev_raw), bio,
//            bio->bi_bdev->bd_dev, bio->bi_sector);

    /* No need to call bio_endio() */
    generic_make_request(bio);
}

static int stackbd_threadfn(void *data)
{
    struct bio *bio;

    set_user_nice(current, -20);

    while (!kthread_should_stop())
    {
        /* wake_up() is after adding bio to list. No need for condition */ 
        wait_event_interruptible(req_event, kthread_should_stop() ||
                !bio_list_empty(&stackbd.bio_list));

        spin_lock_irq(&stackbd.lock);
        if (bio_list_empty(&stackbd.bio_list))
        {
            spin_unlock_irq(&stackbd.lock);
            continue;
        }

        bio = bio_list_pop(&stackbd.bio_list);
        spin_unlock_irq(&stackbd.lock);

        stackbd_io_fn(bio);
    }

    return 0;
}

/*
 * Handle an I/O request.
 */
static blk_qc_t stackbd_make_request(struct request_queue *q, struct bio *bio)
{
    printk("stackbd: make request %-5s block %-12lu #pages %-4hu total-size "
            "%-10u\n", bio_data_dir(bio) == WRITE ? "write" : "read",
            bio->bi_iter.bi_sector, bio->bi_vcnt, bio->bi_iter.bi_size);

//    printk("<%p> Make request %s %s %s\n", bio,
//           bio->bi_rw & REQ_SYNC ? "SYNC" : "",
//           bio->bi_rw & REQ_FLUSH ? "FLUSH" : "",
//           bio->bi_rw & REQ_NOIDLE ? "NOIDLE" : "");
//
    spin_lock_irq(&stackbd.lock);
    if (!stackbd.bdev_raw)
    {
        printk("stackbd: Request before bdev_raw is ready, aborting\n");
        goto abort;
    }
    if (!stackbd.is_active)
    {
        printk("stackbd: Device not active yet, aborting\n");
        goto abort;
    }
    bio_list_add(&stackbd.bio_list, bio);
    wake_up(&req_event);
    spin_unlock_irq(&stackbd.lock);
    //FIXME add a lock
    log_write(current->pid, bio_data_dir(bio), bio->bi_iter.bi_sector, bio->bi_iter.bi_size);

    return BLK_QC_T_NONE;

abort:
    spin_unlock_irq(&stackbd.lock);
    printk("<%p> Abort request\n\n", bio);
    bio_io_error(bio);
    return BLK_QC_T_NONE;
}

static struct block_device *stackbd_bdev_open(char dev_path[])
{
    /* Open underlying device */
    struct block_device *bdev_raw = lookup_bdev(dev_path, 0);
    printk("Opened %s\n", dev_path);

    if (IS_ERR(bdev_raw))
    {
        printk("stackbd: error opening raw device <%lu>\n", PTR_ERR(bdev_raw));
        return NULL;
    }

    if (!bdget(bdev_raw->bd_dev))
    {
        printk("stackbd: error bdget()\n");
        return NULL;
    }

    if (blkdev_get(bdev_raw, STACKBD_BDEV_MODE, &stackbd))
    {
        printk("stackbd: error blkdev_get()\n");
        bdput(bdev_raw);
        return NULL;
    }

    return bdev_raw;
}

static int stackbd_start(char dev_path[])
{
    unsigned max_sectors;

    if (!(stackbd.bdev_raw = stackbd_bdev_open(dev_path)))
        return -EFAULT;

    /* Set up our internal device */
    stackbd.capacity = get_capacity(stackbd.bdev_raw->bd_disk);
    printk("stackbd: Device real capacity: %lu\n", stackbd.capacity);

    set_capacity(stackbd.gd, stackbd.capacity);

    max_sectors = queue_max_hw_sectors(bdev_get_queue(stackbd.bdev_raw));
    blk_queue_max_hw_sectors(stackbd.queue, max_sectors);
    printk("stackbd: Max sectors: %u\n", max_sectors);

    stackbd.thread = kthread_create(stackbd_threadfn, NULL,
           stackbd.gd->disk_name);
    if (IS_ERR(stackbd.thread))
    {
        printk("stackbd: error kthread_create <%lu>\n",
               PTR_ERR(stackbd.thread));
        goto error_after_bdev;
    }

    printk("stackbd: done initializing successfully\n");
    stackbd.is_active = 1;
    wake_up_process(stackbd.thread);

    return 0;

error_after_bdev:
    blkdev_put(stackbd.bdev_raw, STACKBD_BDEV_MODE);
    bdput(stackbd.bdev_raw);

    return -EFAULT;
}

static int stackbd_ioctl(struct block_device *bdev, fmode_t mode,
		     unsigned int cmd, unsigned long arg)
{
    char dev_path[80];
	void __user *argp = (void __user *)arg;    

    switch (cmd)
    {
    case STACKBD_DO_IT:
        printk("\n*** DO IT!!!!!!! ***\n\n");

        if (copy_from_user(dev_path, argp, sizeof(dev_path)))
            return -EFAULT;

        return stackbd_start(dev_path);
    default:
        return -ENOTTY;
    }
}

/*
 * The HDIO_GETGEO ioctl is handled in blkdev_ioctl(), which
 * calls this. We need to implement getgeo, since we can't
 * use tools such as fdisk to partition the drive otherwise.
 */
int stackbd_getgeo(struct block_device * block_device, struct hd_geometry * geo)
{
	long size;

	/* We have no real geometry, of course, so make something up. */
	size = stackbd.capacity * (LOGICAL_BLOCK_SIZE / KERNEL_SECTOR_SIZE);
	geo->cylinders = (size & ~0x3f) >> 6;
	geo->heads = 4;
	geo->sectors = 16;
	geo->start = 0;
	return 0;
}

/*
 * The device operations structure.
 */
static struct block_device_operations stackbd_ops = {
		.owner  = THIS_MODULE,
		.getgeo = stackbd_getgeo,
        .ioctl  = stackbd_ioctl,
};

static int __init stackbd_init(void)
{
	/* Set up our internal device */
	spin_lock_init(&stackbd.lock);

	/* blk_alloc_queue() instead of blk_init_queue() so it won't set up the
     * queue for requests.
     */
    if (!(stackbd.queue = blk_alloc_queue(GFP_KERNEL)))
    {
        printk("stackbd: alloc_queue failed\n");
        return -EFAULT;
    }

    blk_queue_make_request(stackbd.queue, stackbd_make_request);
	blk_queue_logical_block_size(stackbd.queue, LOGICAL_BLOCK_SIZE);

	/* Get registered */
	if ((major_num = register_blkdev(major_num, STACKBD_NAME)) < 0)
    {
		printk("stackbd: unable to get major number\n");
		goto error_after_alloc_queue;
	}

	/* Gendisk structure */
	if (!(stackbd.gd = alloc_disk(16)))
		goto error_after_redister_blkdev;
	stackbd.gd->major = major_num;
	stackbd.gd->first_minor = 0;
	stackbd.gd->fops = &stackbd_ops;
	stackbd.gd->private_data = &stackbd;
	strcpy(stackbd.gd->disk_name, STACKBD_NAME_0);
	stackbd.gd->queue = stackbd.queue;
	add_disk(stackbd.gd);

  // initiate block level tracing
  logs = kmalloc(LOG_ENTRIES_BUFFER_SIZE * sizeof(log_t), GFP_KERNEL);
  if(!logs){
    printk(KERN_ERR "Error allocationg log buffer");
    goto error_after_alloc_queue;
  }

  trace_dev_major = register_chrdev(0, MY_TRACE_MODULE, &my_trace_fops);

    printk("stackbd: init done\n");

	return 0;

error_after_redister_blkdev:
	unregister_blkdev(major_num, STACKBD_NAME);
error_after_alloc_queue:
    blk_cleanup_queue(stackbd.queue);

	return -EFAULT;
}

static void __exit stackbd_exit(void)
{
    printk("stackbd: exit\n");

    if (stackbd.is_active)
    {
        kthread_stop(stackbd.thread);
        blkdev_put(stackbd.bdev_raw, STACKBD_BDEV_MODE);
        bdput(stackbd. bdev_raw);
    }

    kfree(logs);
    unregister_chrdev(trace_dev_major, MY_TRACE_MODULE);

	del_gendisk(stackbd.gd);
	put_disk(stackbd.gd);
	unregister_blkdev(major_num, STACKBD_NAME);
	blk_cleanup_queue(stackbd.queue);
}

module_init(stackbd_init);
module_exit(stackbd_exit);
