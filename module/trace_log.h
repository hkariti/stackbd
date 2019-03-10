#ifndef TRACE_LOG_H
#define TRACE_LOG_H
struct trace_log;
int log_init(unsigned int, unsigned int, char*);
void log_destroy(void);
int log_entries_count(void);
int log_clients_count(void);
void log_increment_read_head(int);
void log_increment_write_head(void);
void log_write_entry(void*);
#endif
