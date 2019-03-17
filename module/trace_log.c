#include <linux/kernel.h>
#include <linux/fdtable.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>

#include "trace_log.h"

struct trace_log {
    void* entries;
    unsigned int log_read_head, log_write_head;
    unsigned int overruns;
    unsigned int logger_clients;
    unsigned int entry_count;
    unsigned int entry_size;
    char* device_name;
    int dev_major;
    struct mutex lock;
};

static int logger_open(struct inode*, struct file*);
static ssize_t logger_read(struct file*, char*, size_t, loff_t*);
static int logger_release(struct inode*, struct file*);
static struct file_operations log_fops = {
    .read = logger_read,
    .open = logger_open,
    .release = logger_release
};

static struct trace_log log;

int log_init(unsigned int entry_size, unsigned int entry_count, char* device_name) {
    log.entry_count = entry_count;
    log.entry_size = entry_size;
    log.entries = kmalloc(entry_size * entry_count, GFP_KERNEL);
    mutex_init(&log.lock);
    log.dev_major = register_chrdev(0, device_name, &log_fops);
    log.device_name = device_name;
    if (!log.entries || log.dev_major == -1) {
        log_destroy();
        return -1;
    }
    return 0;
};

void log_destroy() {
    mutex_destroy(&log.lock);
    unregister_chrdev(log.dev_major, log.device_name);
    kfree(log.entries);
}

int log_entries_count() {
    int diff = log.log_write_head - log.log_read_head;
    if (diff < 0)
        diff += log.entry_count;
    return diff;
}

int log_clients_count() {
    return log.logger_clients;
}

// we hold the log lock here
void log_increment_read_head(int count) {
    log.log_read_head = (log.log_read_head + count) % log.entry_count;
}

// we hold the log lock here
void log_increment_write_head() {
    log.log_write_head = (log.log_write_head + 1) % log.entry_count;
    // Check if we rolled over the read head, i.e. overrun
    if (log.log_write_head == log.log_read_head) {
        log_increment_read_head(1);
        log.overruns++;
        printk(KERN_WARNING "%s: log overrun", log.device_name);
    }
};

void log_write_entry(void* data) {
    void* log_entry;
    mutex_lock(&log.lock);
    log_entry = log.entries + log.log_write_head * log.entry_size;
    log_increment_write_head();
    memcpy(log_entry, data, log.entry_size);
    mutex_unlock(&log.lock);
}

static int logger_open(struct inode* in, struct file* fd) {
    mutex_lock(&log.lock);
    log.logger_clients++;
    mutex_unlock(&log.lock);
    printk(KERN_DEBUG "%s: added reader, number is now %d", log.device_name, log.logger_clients);
    return 0;
}

static char* _logger_read(char *buffer, size_t count) {
    copy_to_user(buffer, log.entries + log.log_read_head * log.entry_size, count * log.entry_size);
    log_increment_read_head(count);
    return buffer + count * log.entry_size;
}

static ssize_t logger_read(struct file *fd, char *buffer, size_t byte_count, loff_t *offset) {
    size_t first_read_size, second_read_size;
    size_t count = byte_count / log.entry_size;
    size_t log_count;
    mutex_lock(&log.lock);
    log_count = log_entries_count();
    if (count > log_count)
        count = log_count;
    first_read_size = count;
    second_read_size = 0;
    if (log.log_read_head + count > log.entry_count) {
        first_read_size = log.entry_count - log.log_read_head;
        second_read_size = count - first_read_size;
    }
    buffer = _logger_read(buffer, first_read_size);
    buffer = _logger_read(buffer, second_read_size);
    mutex_unlock(&log.lock);
    return count * log.entry_size;
}

static int logger_release(struct inode *in, struct file *fd) {
    mutex_lock(&log.lock);
    log.logger_clients--;
    mutex_unlock(&log.lock);
    printk(KERN_DEBUG "%s: removed reader, number is now %d", log.device_name, log.logger_clients);
    return 0;
}
