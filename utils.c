// utils.c
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include "utils.h"
#include "packets.h"

void perror_exit(const char *message) {
    perror(message);
    exit(EXIT_FAILURE);
}

uint64_t get_timestamp_millis() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    // Convert seconds to milliseconds and add microseconds divided by 1000
    return (uint64_t)(tv.tv_sec) * 1000 + (tv.tv_usec) / 1000;
}

double format_size_with_unit(uint64_t bytes, char *unit) {
    const uint64_t KB = 1024;
    const uint64_t MB = 1024 * KB;
    const uint64_t GB = 1024 * MB;

    double value;

    if (bytes >= GB) {
        value = (double)bytes / GB;
        strcpy(unit, "GB");
    } else if (bytes >= MB) {
        value = (double)bytes / MB;
        strcpy(unit, "MB");
    } else if (bytes >= KB) {
        value = (double)bytes / KB;
        strcpy(unit, "kB");
    } else {
        value = (double)bytes;
        strcpy(unit, "B");
    }
    return value;
}


void delay_microseconds(long delay_us) {
    struct timespec start, current;
    
    // Get the current time
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    long elapsed_us = 0;
    
    while (elapsed_us < delay_us) {
        // Get the current time again
        clock_gettime(CLOCK_MONOTONIC, &current);
        
        // Calculate the elapsed time in microseconds
        elapsed_us = (current.tv_sec - start.tv_sec) * 1000000L 
                     + (current.tv_nsec - start.tv_nsec) / 1000L;
    }
}