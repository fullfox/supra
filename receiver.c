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

void receiver_run(int argc, char *argv[]) {

    struct sockaddr_in local_addr;
    int sockfd = create_and_bind_udp_socket(&local_addr);

    NetStats netStats;
    netStats.role = 1;
    netStats.total_bytes_transfered = 0;
    netStats.delta_bytes_transfered = 0;
    netStats.t1 = 0;

    struct sockaddr_in sender_addr;
    get_destination(&sender_addr, argc, argv);

    // only required for receiver, no ack, just 3 outgoing udp
    if(udp_hole_punch(sockfd, &sender_addr) == -1){
        exit(EXIT_FAILURE);
    }

    // Receive file metadata
    InitPacket initPacket;
    while(1){
        ssize_t n = recvfrom(sockfd, &initPacket, sizeof(initPacket), 0, NULL, NULL);
        if(initPacket.type == INIT && n == sizeof(initPacket)) break;
    }
    
    uint64_t file_size = initPacket.file_size;
    netStats.file_size = file_size;
    uint32_t frame_size = initPacket.frame_size;
    printf("Receiving file size: %lu, frame size: %u\n", file_size, frame_size);


    // Open file for writing
    FILE *fp = fopen("received_file", "wb");
    if (!fp) {
        fprintf(stderr, "Failed to open file for writing\n");
        exit(EXIT_FAILURE);
    }

    // Pre-allocate file size
    fseek(fp, file_size - 1, SEEK_SET);
    fputc('\0', fp);
    fflush(fp);

    // Initialize tracking variables
    uint32_t last_nack_index = 0;
    uint64_t total_packets = (file_size + frame_size - 1) / frame_size;
    uint8_t *received_packets = calloc(total_packets, sizeof(uint8_t));
    if (!received_packets) {
        perror_exit("Failed to allocate memory for received packets");
    }

    uint8_t *buffer = malloc(frame_size + sizeof(ChunkPacketHeader));
    if (!buffer) {
        perror_exit("Failed to allocate buffer");
    }


    Packet initAckPacket;
    initAckPacket.type=INIT;

    // Send periodically the init ack packet ( 'pls start' packet )
    pthread_t periodic_sender_thread;
    pthread_create(&periodic_sender_thread, NULL, periodic_sender_routine, &(periodic_sender_context_t){
        .sockfd = sockfd,
        .addr = (struct sockaddr*)&sender_addr,
        .addr_len = sizeof(sender_addr),
        .data = (uint8_t*)&initAckPacket,
        .datalen = sizeof(initAckPacket)
    });
    pthread_detach(periodic_sender_thread);

    // Start network stats routine
    pthread_t netstats_thread;
    pthread_create(&netstats_thread, NULL, netstats_routine, &netStats);
    pthread_detach(netstats_thread);


    // Start slowdown packet listenner 
    pthread_t slowdown_thread;
    pthread_create(&slowdown_thread, NULL, slowdown_routine, &netStats);
    pthread_detach(slowdown_thread);



    int complete = 0;
    while (!complete) {

        // Listen for file chunks or check packets
        ssize_t n = recvfrom(sockfd, buffer, frame_size+sizeof(ChunkPacketHeader), 0, NULL, NULL);
        if (n < sizeof(Packet)) {
            fprintf(stderr, "Received an incomplete packet\n");
            continue;
        }

        Packet * packet = (Packet *) buffer;

        if(packet->type == FILE_CHUNK && n >= sizeof(ChunkPacketHeader)){
            if (periodic_sender_thread) {
                pthread_cancel(periodic_sender_thread);
                periodic_sender_thread = 0;
            }

            ChunkPacketHeader *header = (ChunkPacketHeader *) buffer;

            // Validate the packet data length
            if (header->data_len > frame_size || n < sizeof(ChunkPacketHeader) + header->data_len) {
                fprintf(stderr, "Invalid packet size or corrupted data\n");
                continue;
            }

            // Check if the sequence number is valid
            if (header->seq_num >= total_packets) {
                fprintf(stderr, "Received out-of-range packet %u\n", header->seq_num);
                continue;
            }

            // Process only if the packet hasn't been received yet
            if (!received_packets[header->seq_num]) {
                netStats.delta_bytes_transfered += header->data_len;
                
                uint64_t offset = (uint64_t)header->seq_num * frame_size;

                // Write data directly to file at the correct offset
                fseek(fp, offset, SEEK_SET);
                fwrite(buffer + sizeof(ChunkPacketHeader), 1, header->data_len, fp);
                fflush(fp);

                received_packets[header->seq_num] = 1;
            }

        } else if(packet->type == CHECK) { // SEND NACK
            
             uint32_t requested_total = 0;
            uint32_t stop_nack_index = (last_nack_index+total_packets-1)%total_packets;
            for (size_t j = 0; j < 10; j++) { //todo optimize 10
                uint32_t missing_count = 0;
                uint32_t missing_packets[MAX_NACK];
                for (uint32_t i = last_nack_index; i < total_packets+last_nack_index; i++) {
                    if (!received_packets[i%total_packets]) {
                        missing_packets[missing_count++] = i%total_packets;

                        if (missing_count == MAX_NACK || i == stop_nack_index) {
                            last_nack_index = (i+1)%total_packets;
                            break;
                        }
                    }
                }

                if (missing_count == 0) {
                    complete = 1;
                    break;
                }

                requested_total+=missing_count;
                send_nack(sockfd, &sender_addr, missing_packets, missing_count);
                if(stop_nack_index+1 == last_nack_index){
                    break;
                }
            }
            printf("Requested %i missing packet.\n",requested_total);
        }
    }

    printf("File transfer complete!\n");
    pthread_cancel(netstats_thread);
    free(buffer);
    free(received_packets);
    fclose(fp);
    close(sockfd);
}