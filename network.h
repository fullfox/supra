#ifndef NETWORK_H
#define NETWORK_H

#include <netinet/in.h>

enum net_stats_role {
  SENDER,
  RECEIVER
};

typedef struct {
    uint8_t role; // 0 = sender, 1 = receiver
    uint64_t file_size;
    uint64_t total_bytes_transfered;
    uint64_t delta_bytes_transfered; // since t1
    uint64_t t1;
    int sockfd;
    struct sockaddr_in dest_addr;
    socklen_t dest_addr_len;
    uint64_t sleep_delay;
    uint64_t current_bitrate;
} NetStats;

typedef struct {
    int sockfd;
    struct sockaddr *addr;
    socklen_t addr_len;
    uint8_t * data;
    int datalen;
    volatile int *state;
} periodic_sender_context_t;

int create_and_bind_udp_socket(struct sockaddr_in *local_addr);

void get_destination(struct sockaddr_in *dest_addr, int argc, char *argv[]);

int udp_hole_punch(int sockfd, struct sockaddr_in *dest_addr);

void *periodic_sender_routine(void *arg);

void * netstats_routine(void *arg);

void * slowdown_routine(void *arg);


#endif
