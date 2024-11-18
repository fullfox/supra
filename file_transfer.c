// file_transfer.c
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include "file_transfer.h"
#include "packets.h"

#define UDP_PAYLOAD_SIZE 1400 // Optimized. Max UDP payload size (65,507 bytes)

void send_nack(int sockfd, struct sockaddr_in *sender_addr, uint32_t *missing_packets, uint32_t missing_count) {
    NackPacket nack;
    nack.type = NACK;
    nack.count = missing_count;

    for (uint32_t i = 0; i < missing_count; i++) {
        nack.missing[i] = missing_packets[i];
    }

    ssize_t sent_bytes = sendto(sockfd, &nack, sizeof(nack), 0, (struct sockaddr *)sender_addr, sizeof(*sender_addr));

    if (sent_bytes < 0) {
        perror("Failed to send NACK");
    }
}

int send_file_chunk(int sockfd, struct sockaddr_in *dest_addr, socklen_t dest_addr_len, FILE *fp, uint32_t seq_num, size_t frame_size, uint8_t * buffer, NetStats* netStats) {
    ChunkPacketHeader *header = (ChunkPacketHeader *)buffer;
    header->type = FILE_CHUNK;
    header->seq_num = seq_num;

    fseek(fp, (uint64_t)seq_num * frame_size, SEEK_SET);
    size_t bytes_read = fread(buffer + sizeof(ChunkPacketHeader), 1, frame_size, fp);

    if (bytes_read > 0) {
        header->data_len = bytes_read;
        size_t total_size = sizeof(ChunkPacketHeader) + bytes_read;
        sendto(sockfd, buffer, total_size, 0, (struct sockaddr*) dest_addr, dest_addr_len);
        netStats->delta_bytes_transfered+=total_size;
    }

    return (bytes_read > 0) ? 0 : -1;
}

/*

sender - receiver
-> init ->
<- ack init <-

-> data ->
-> data ->
-> data ->

-> check ->
<- nack <-
-> data ->

-> check ->
<- nack <-
-> data ->

<- good <-

*/