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
#include "network.h"
#include "utils.h"
#include "packets.h"

#define UDP_PAYLOAD_SIZE 1400 // Optimized. Max UDP payload size (65,507 bytes)


// Sender implementation
void sender_run(const char *file_path, int argc, char *argv[]) {
    struct sockaddr_in local_addr;
    int sockfd = create_and_bind_udp_socket(&local_addr);

    struct sockaddr_in dest_addr;
    get_destination(&dest_addr, argc, argv);
    socklen_t dest_addr_len = sizeof(dest_addr);

    NetStats netStats;
    netStats.role = 0;
    netStats.total_bytes_transfered = 0;
    netStats.delta_bytes_transfered = 0;
    netStats.t1 = 0;
    netStats.sockfd = sockfd;
    netStats.dest_addr = dest_addr;
    netStats.dest_addr_len = dest_addr_len;
    netStats.sleep_delay = 0;

    if(udp_hole_punch(sockfd, &dest_addr) == -1){
        exit(EXIT_FAILURE);
    }

    int frame_size = 1420 - sizeof(ChunkPacketHeader);

    // Open the file
    FILE *fp = fopen(file_path, "rb");
    if (!fp) {
        perror_exit("Failed to open file");
    }

    // Get file size
    fseek(fp, 0, SEEK_END);
    uint64_t file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    printf("File size:%lu\n", file_size);
    netStats.file_size = file_size;


    // Send file metadata
    InitPacket initPacket;
    initPacket.type = INIT;
    initPacket.file_size = file_size;
    initPacket.frame_size = frame_size;

    // Send periodically the init packet
    pthread_t periodic_sender_thread;
    pthread_create(&periodic_sender_thread, NULL, periodic_sender_routine, &(periodic_sender_context_t){
        .sockfd = sockfd,
        .addr = (struct sockaddr*)&dest_addr,
        .addr_len = sizeof(dest_addr),
        .data = (uint8_t*)&initPacket,
        .datalen = sizeof(initPacket)
    });
    pthread_detach(periodic_sender_thread);

    // Wait for the receiver to ack
    Packet initAckPacket;
    while(1) {
        ssize_t bytes_received = recvfrom(sockfd, &initAckPacket, sizeof(initAckPacket), 0, (struct sockaddr *)&dest_addr, &dest_addr_len);
        if (bytes_received > 0) {
            if (initAckPacket.type == INIT) {
                printf("Starting transmission...\n");
                pthread_cancel(periodic_sender_thread);
                break;
            }
        }
    }

    // Start network stats routine
    pthread_t netstats_thread;
    pthread_create(&netstats_thread, NULL, netstats_routine, &netStats);
    pthread_detach(netstats_thread);

    // Send file data
    uint32_t seq_num = 0;
    size_t bytes_read;
    uint8_t *buffer = malloc(frame_size + sizeof(ChunkPacketHeader));
    while ((bytes_read = send_file_chunk(sockfd, &dest_addr, dest_addr_len, fp, seq_num, frame_size, buffer, &netStats)) == 0) {
        seq_num++;
        if(netStats.sleep_delay > 0) delay_microseconds(netStats.sleep_delay);
    }
    


    // Send checks, receive nacks, and retransmit
    Packet checkPacket;
    checkPacket.type = CHECK;
    int complete = 0;

    while (!complete) {
        // Send periodically the check packet
        pthread_create(&periodic_sender_thread, NULL, periodic_sender_routine, &(periodic_sender_context_t){
            .sockfd = sockfd,
            .addr = (struct sockaddr*)&dest_addr,
            .addr_len = sizeof(dest_addr),
            .data = (uint8_t*)&checkPacket,
            .datalen = sizeof(checkPacket)
        });
        pthread_detach(periodic_sender_thread);

        // Listen for nacks
        NackPacket NackPacket;
        while (1) {
            ssize_t bytes_received = recvfrom(sockfd, &NackPacket, sizeof(NackPacket), 0, (struct sockaddr*)&dest_addr, &dest_addr_len);
            if (bytes_received > 0) {
                if (NackPacket.type == NACK) {
                    pthread_cancel(periodic_sender_thread);
                    if (NackPacket.count == 0) {
                        complete = 1;
                    }

                    for (uint32_t i = 0; i < NackPacket.count; i++) {
                        uint32_t seq_num = NackPacket.missing[i];
                        if (send_file_chunk(sockfd, &dest_addr, dest_addr_len, fp, seq_num, frame_size, buffer, &netStats) < 0) {
                            fprintf(stderr, "Failed to retransmit packet %u\n", seq_num);
                        }
                    }

                    break;
                }
            }
        }
    }

    pthread_cancel(netstats_thread);
    free(buffer);
    fclose(fp);
    close(sockfd);
}