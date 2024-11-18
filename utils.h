// utils.h
#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>


uint64_t get_timestamp_millis();
double format_size_with_unit(uint64_t bytes, char *unit);
void perror_exit(const char *message);
void *netstats_routine(void *arg);
void delay_microseconds(long delay_us);

#endif // UTILS_H
