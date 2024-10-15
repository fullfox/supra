#ifndef NETWORK_H
#define NETWORK_H

#include <netinet/in.h>

int create_and_bind_udp_socket(struct sockaddr_in *local_addr);

void get_destination(struct sockaddr_in *dest_addr, int argc, char *argv[]);

int udp_hole_punch(int sockfd, struct sockaddr_in *dest_addr);

void periodic_sender(int sockfd, struct sockaddr *addr, socklen_t addr_len, uint8_t* data, int datalen, volatile int *state);
void *periodic_sender_routine(void *arg);

#endif
