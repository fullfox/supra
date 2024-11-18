#ifndef FILE_TRANSFER_H
#define FILE_TRANSFER_H

#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include "utils.h"
#include "network.h"

// Sender functions
void sender_run(const char *file_path, int argc, char *argv[]);

// Receiver functions
void receiver_run(int argc, char *argv[]);

// Utility functions
void send_nack(int sockfd, struct sockaddr_in *sender_addr, uint32_t *missing_packets, uint32_t missing_count);
int send_file_chunk(int sockfd, struct sockaddr_in *dest_addr, socklen_t dest_addr_len, FILE *fp, uint32_t seq_num, size_t packet_size, uint8_t *buffer, NetStats * netstats);

#endif // FILE_TRANSFER_H
