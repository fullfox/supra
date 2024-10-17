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
#include "network.h"
#include "utils.h"


#define UDP_PAYLOAD_SIZE 1400 // Optimized. Max UDP payload size (65,507 bytes)
#define MAX_NACK 350 // 4 byte per seq, NACKPacket about 1408 bytes

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

typedef enum {
    INIT,
    FILE_CHUNK,
    CHECK,
    NACK
} PacketType;

typedef struct {
    PacketType type;
} Packet;

typedef struct {
    PacketType type;
    uint32_t seq_num;
    uint32_t data_len;
    // Data follows
} PacketHeader;

typedef struct {
    PacketType type;
    uint64_t file_size;
    uint32_t frame_size;
} InitPacket;

typedef struct {
    PacketType type;
    uint32_t count;
    uint32_t missing[MAX_NACK];
} NACKPacket;


// Sender implementation
void sender_run(const char *file_path, int argc, char *argv[]) {
    volatile int periodic_sender_state = 0;

    NetStats netStats;
    netStats.total_bytes_transfered = 0;
    netStats.delta_bytes_transfered = 0;
    netStats.t1 = 0;

    struct sockaddr_in local_addr;
    int sockfd = create_and_bind_udp_socket(&local_addr);

    struct sockaddr_in dest_addr;
    get_destination(&dest_addr, argc, argv);
    socklen_t dest_addr_len = sizeof(dest_addr);

    if(udp_hole_punch(sockfd, &dest_addr) == -1){
        exit(EXIT_FAILURE);
    }

    int frame_size = 1420 - sizeof(PacketHeader);
    long sleep_delay = atoi(argv[0]);

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
    periodic_sender_state = 1;
    periodic_sender(sockfd, (struct sockaddr*) &dest_addr, sizeof(dest_addr), (uint8_t*)&initPacket, sizeof(initPacket), &periodic_sender_state);
    Packet initAckPacket;
    while(1) {
        ssize_t bytes_received = recvfrom(sockfd, &initAckPacket, sizeof(initAckPacket), 0, (struct sockaddr *)&dest_addr, &dest_addr_len);
        if (bytes_received > 0) {
            if (initAckPacket.type == INIT) {
                printf("Starting transmission...\n");
                periodic_sender_state = 0;
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
    uint8_t *buffer = malloc(frame_size + sizeof(PacketHeader));
    while ((bytes_read = send_file_chunk(sockfd, &dest_addr, dest_addr_len, fp, seq_num, frame_size, buffer, &netStats)) == 0) {
        seq_num++;
        if(sleep_delay>0) delay_microseconds(sleep_delay);
    }
    


    // Send checks, receive nacks, and retransmit
    Packet checkPacket;
    checkPacket.type = CHECK;
    int complete = 0;

    while (!complete) {
        periodic_sender_state = 1;
        periodic_sender(sockfd, (struct sockaddr*)&dest_addr, sizeof(dest_addr), (uint8_t*) &checkPacket, sizeof(checkPacket), &periodic_sender_state);
        NACKPacket nackPacket;
        while (1) {
            ssize_t bytes_received = recvfrom(sockfd, &nackPacket, sizeof(nackPacket), 0, (struct sockaddr*)&dest_addr, &dest_addr_len);
            if (bytes_received > 0) {
                if (nackPacket.type == NACK) {
                    periodic_sender_state = 0;
                    if (nackPacket.count == 0) {
                        complete = 1;
                    }

                    for (uint32_t i = 0; i < nackPacket.count; i++) {
                        uint32_t seq_num = nackPacket.missing[i];
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



// Receiver implementation
void receiver_run(int argc, char *argv[]) {

    NetStats netStats;
    netStats.total_bytes_transfered = 0;
    netStats.delta_bytes_transfered = 0;
    netStats.t1 = 0;

    struct sockaddr_in local_addr;
    int sockfd = create_and_bind_udp_socket(&local_addr);

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

    uint8_t *buffer = malloc(frame_size + sizeof(PacketHeader));
    if (!buffer) {
        perror_exit("Failed to allocate buffer");
    }


    Packet initAckPacket;
    initAckPacket.type=INIT;

    volatile int periodic_sender_state = 1;
    periodic_sender(sockfd, (struct sockaddr*) &sender_addr, sizeof(sender_addr), (uint8_t*) &initAckPacket, sizeof(initAckPacket), &periodic_sender_state);


    // Start network stats routine
    pthread_t netstats_thread;
    pthread_create(&netstats_thread, NULL, netstats_routine, &netStats);
    pthread_detach(netstats_thread);

    int complete = 0;
    while (!complete) {
        ssize_t n = recvfrom(sockfd, buffer, frame_size+sizeof(PacketHeader), 0, NULL, NULL);
        if (n < sizeof(Packet)) {
            fprintf(stderr, "Received an incomplete packet\n");
            continue;
        }

        Packet * packet = (Packet *) buffer;

        if(packet->type == FILE_CHUNK && n >= sizeof(PacketHeader)){
            periodic_sender_state = 0;
            PacketHeader *header = (PacketHeader *) buffer;

            // Validate the packet data length
            if (header->data_len > frame_size || n < sizeof(PacketHeader) + header->data_len) {
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
                fwrite(buffer + sizeof(PacketHeader), 1, header->data_len, fp);
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


void send_nack(int sockfd, struct sockaddr_in *sender_addr, uint32_t *missing_packets, uint32_t missing_count) {
    NACKPacket nack;
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
    PacketHeader *header = (PacketHeader *)buffer;
    header->type = FILE_CHUNK;
    header->seq_num = seq_num;

    fseek(fp, (uint64_t)seq_num * frame_size, SEEK_SET);
    size_t bytes_read = fread(buffer + sizeof(PacketHeader), 1, frame_size, fp);

    if (bytes_read > 0) {
        header->data_len = bytes_read;
        size_t total_size = sizeof(PacketHeader) + bytes_read;
        sendto(sockfd, buffer, total_size, 0, (struct sockaddr*) dest_addr, dest_addr_len);
        netStats->delta_bytes_transfered+=total_size;
    }

    return (bytes_read > 0) ? 0 : -1;
}