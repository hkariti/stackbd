#include <linux/types.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdbool.h>

#define DEV "/dev/block_trace"
#define BUFFER_SIZE 100

typedef struct _log_t {
    unsigned int pid;
    int write;
    unsigned long offset;
    size_t size;

} log_t;
int main() {
    int fd;
    ssize_t entries_read;
    log_t buffer[BUFFER_SIZE];

    fd = open(DEV, O_RDONLY);
    if (fd < 0) {
        printf("Error opening file\n");
        return -1;
    }

    while (1) {
        entries_read = read(fd, buffer, BUFFER_SIZE);
        if (entries_read < 0) {
            printf("Error reading from file\n");
            close(fd);
            return -1;
        }
        for (int i = 0; i < entries_read; i++) {
            printf("PID: %d offset: %ld size: %ld write: %d\n", buffer[i].pid, buffer[i].offset, buffer[i].size, buffer[i].write);
        }
        sleep(1);
    }
    return 0;
}
