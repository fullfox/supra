// utils.h
#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>

typedef struct {
    uint64_t file_size;
    uint64_t total_bytes_transfered;
    uint64_t delta_bytes_transfered;
    uint64_t t1;
} NetStats;


uint64_t get_timestamp_millis();
double format_size_with_unit(uint64_t bytes, char *unit);
void perror_exit(const char *message);
void *netstats_routine(void *arg);
void delay_microseconds(long delay_us);

#endif // UTILS_H
