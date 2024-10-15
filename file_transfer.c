// file_transfer.c
#include <stdio.h>
#include <stdlib.h>
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
    DATA,
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

    struct sockaddr_in local_addr;
    int sockfd = create_and_bind_udp_socket(&local_addr);

    struct sockaddr_in dest_addr;
    get_destination(&dest_addr, argc, argv);
    socklen_t dest_addr_len = sizeof(dest_addr);

    if(udp_hole_punch(sockfd, &dest_addr) == -1){
        exit(EXIT_FAILURE);
    }

    int frame_size = UDP_PAYLOAD_SIZE - sizeof(PacketHeader);

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


    // Send file data
    uint32_t seq_num = 0;
    size_t bytes_read;
    uint8_t *buffer = malloc(frame_size + sizeof(PacketHeader));
    while ((bytes_read = transmit_file_chunk(sockfd, &dest_addr, dest_addr_len, fp, seq_num, frame_size, buffer)) == 0) {
        seq_num++;
        usleep(1);
    }
    free(buffer);


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
                    complete = parse_nack_and_retransmit(sockfd, &dest_addr, dest_addr_len, fp, (uint8_t*) &nackPacket, frame_size);
                    break;
                }
            }
        }
    }

    fclose(fp);
    close(sockfd);
}



// Receiver implementation
void receiver_run(int argc, char *argv[]) {

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
    uint64_t received_size = 0;
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

    int tick = 0;
    uint64_t t1 = get_timestamp_millis();
    uint64_t last_received = 0;
    while (1) {
        ssize_t n = recvfrom(sockfd, buffer, frame_size+sizeof(PacketHeader), 0, NULL, NULL);
        if (n < sizeof(Packet)) {
            fprintf(stderr, "Received an incomplete packet\n");
            continue;
        }

        Packet * packet = (Packet *) buffer;

        if(packet->type == DATA && n >= sizeof(PacketHeader)){
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
                received_size+= header->data_len;
                tick++;

                if (tick>10000){
                    // PRINT DEBUG
                    uint64_t t2 = get_timestamp_millis();
                    uint64_t delta_t = t2 - t1;
                    t1 = t2;

                    uint64_t delta_bytes = received_size - last_received;
                    last_received = received_size;

                    uint64_t bitrate = 1000*delta_bytes/delta_t;

                    char unit[3];
                    double conv_bitrate = format_size_with_unit(bitrate, unit);

                    tick = 0;
                    printf("received %.2f, bitrate %.1f %s/s\n", (double)100*received_size/ (double)file_size, conv_bitrate, unit);
                }
                
                uint64_t offset = (uint64_t)header->seq_num * frame_size;

                // Write data directly to file at the correct offset
                fseek(fp, offset, SEEK_SET);
                fwrite(buffer + sizeof(PacketHeader), 1, header->data_len, fp);
                fflush(fp);

                received_packets[header->seq_num] = 1;
            }

        } else if(packet->type == CHECK) { // SEND NACK todo more than 1000
            uint32_t missing_packets[MAX_NACK];
            uint32_t missing_count = 0;

            for (uint32_t i = 0; i < total_packets; i++) {
                if (!received_packets[i]) {
                    missing_packets[missing_count++] = i;
                    if (missing_count == MAX_NACK) break;
                }
            }

            printf("Check received, missing %i\n", missing_count);
            send_nack(sockfd, &sender_addr, missing_packets, missing_count);
            if (missing_count == 0) {
                printf("File transfer complete!\n");
                break;
            }
        }
    }

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

int parse_nack_and_retransmit(int sockfd, struct sockaddr_in *dest_addr, socklen_t dest_addr_len, FILE *fp, uint8_t * packet, size_t frame_size) {
    NACKPacket * nackPacket = (NACKPacket *) packet;
    if (nackPacket->count == 0) {
        return 1;
    }

    uint8_t *buffer = malloc(frame_size + sizeof(PacketHeader));
    for (uint32_t i = 0; i < nackPacket->count; i++) {
        uint32_t seq_num = nackPacket->missing[i];
        if (transmit_file_chunk(sockfd, dest_addr, dest_addr_len, fp, seq_num, frame_size, buffer) < 0) {
            fprintf(stderr, "Failed to retransmit packet %u\n", seq_num);
        }
    }
    free(buffer);

    return 0;
}

int transmit_file_chunk(int sockfd, struct sockaddr_in *dest_addr, socklen_t dest_addr_len, FILE *fp, uint32_t seq_num, size_t frame_size, uint8_t * buffer) {
    PacketHeader *header = (PacketHeader *)buffer;
    header->type = DATA;
    header->seq_num = seq_num;

    fseek(fp, (uint64_t)seq_num * frame_size, SEEK_SET);
    size_t bytes_read = fread(buffer + sizeof(PacketHeader), 1, frame_size, fp);

    if (bytes_read > 0) {
        header->data_len = bytes_read;
        size_t total_size = sizeof(PacketHeader) + bytes_read;
        sendto(sockfd, buffer, total_size, 0, (struct sockaddr*) dest_addr, dest_addr_len);
    }

    return (bytes_read > 0) ? 0 : -1;
}