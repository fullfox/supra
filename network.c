#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include "network.h"
#include "utils.h"

#define PUNCH_ATTEMPT_MSG "ping"
#define PUNCH_OK_MSG "pong"


int create_and_bind_udp_socket(struct sockaddr_in *local_addr) {
    int sockfd;
    socklen_t addr_len = sizeof(*local_addr);

    // Create UDP socket
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror_exit("Socket creation failed");
    }

    // Initialize local address structure
    memset(local_addr, 0, sizeof(*local_addr));
    local_addr->sin_family = AF_INET;
    local_addr->sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr->sin_port = htons(0); // Let OS choose the port

    // Bind the socket to the local address
    if (bind(sockfd, (struct sockaddr*)local_addr, sizeof(*local_addr)) < 0) {
        perror_exit("Bind failed");
    }

    if (getsockname(sockfd, (struct sockaddr*)local_addr, &addr_len) == -1) {
        perror_exit("getsockname() failed");
    }

    printf("UDP socket bound to port: %d\n", ntohs(local_addr->sin_port));

    return sockfd;
}


void get_destination(struct sockaddr_in *dest_addr, int argc, char *argv[]) {
    char dest_ip[INET_ADDRSTRLEN] = {0};
    int dest_port = 0;

    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--dest-ip") == 0 && i + 1 < argc) {
            strncpy(dest_ip, argv[++i], INET_ADDRSTRLEN - 1);
        } else if (strcmp(argv[i], "--dest-port") == 0 && i + 1 < argc) {
            dest_port = atoi(argv[++i]);
        }
    }

    if (dest_ip[0] == '\0') {
        printf("Enter destination IP: ");
        scanf("%s", dest_ip);
    }

    if (dest_port == 0) {
        printf("Enter destination port: ");
        scanf("%d", &dest_port);
    }

    memset(dest_addr, 0, sizeof(*dest_addr));
    dest_addr->sin_family = AF_INET;
    if (inet_pton(AF_INET, dest_ip, &dest_addr->sin_addr) <= 0) {
        fprintf(stderr, "Invalid address\n");
        exit(EXIT_FAILURE);
    }
    dest_addr->sin_port = htons(dest_port);
}


#define MAX_ATTEMPTS 10

typedef struct {
    int sockfd;
    struct sockaddr_in dest_addr;
    volatile int state; // Shared state: 0 (pending), 1 (success), -1 (timeout)
} udp_punch_context_t;

void* udp_hole_punch_listen_thread(void* arg) {
    udp_punch_context_t* pstate = (udp_punch_context_t*)arg;
    char buf[16] = {0};
    struct sockaddr_in recv_addr;
    socklen_t recv_len = sizeof(recv_addr);

    while (pstate->state != 2) {
        int n = recvfrom(pstate->sockfd, buf, sizeof(buf)-1, 0, (struct sockaddr*)&recv_addr, &recv_len);
        if (n > 0) {
            buf[n] = '\0';
            if (strcmp(buf, PUNCH_ATTEMPT_MSG) == 0 &&
                recv_addr.sin_addr.s_addr == pstate->dest_addr.sin_addr.s_addr &&
                recv_addr.sin_port == pstate->dest_addr.sin_port) {
                pstate->state = 1;
                printf("received ping\n");
            } else if(strcmp(buf, PUNCH_OK_MSG) == 0 &&
                recv_addr.sin_addr.s_addr == pstate->dest_addr.sin_addr.s_addr &&
                recv_addr.sin_port == pstate->dest_addr.sin_port){
                pstate->state = 2;  // Hole punched successfully
                printf("received pong\n");
                return NULL;
            }
        }
    }
    return NULL;
}

int udp_hole_punch(int sockfd, struct sockaddr_in *dest_addr) {
    udp_punch_context_t pstate = {sockfd, *dest_addr, 0};
    pthread_t thread;

    // Start listening thread
    pthread_create(&thread, NULL, udp_hole_punch_listen_thread, &pstate);

    for (int attempts = 0; attempts < MAX_ATTEMPTS; ++attempts) {

        if (pstate.state == 0) {
            sendto(sockfd, PUNCH_ATTEMPT_MSG, strlen(PUNCH_ATTEMPT_MSG), 0, (struct sockaddr*)dest_addr, sizeof(*dest_addr));
        } else if (pstate.state == 1){
            sendto(sockfd, PUNCH_OK_MSG, strlen(PUNCH_OK_MSG), 0, (struct sockaddr*)dest_addr, sizeof(*dest_addr));
        } else if (pstate.state == 2) {
            sendto(sockfd, PUNCH_OK_MSG, strlen(PUNCH_OK_MSG), 0, (struct sockaddr*)dest_addr, sizeof(*dest_addr));
            printf("Hole punched!\n");
            break;
        }
        sleep(1);
    }

    if (pstate.state == 0) {
        pstate.state = -1; // Timeout
        fprintf(stderr, "Failed to punch hole after 10 seconds.\n");
        pthread_cancel(thread); // Kill listening thread on timeout
    }

    pthread_join(thread, NULL); // Clean up thread
    return pstate.state;
}



typedef struct {
    int sockfd;
    struct sockaddr *addr;
    socklen_t addr_len;
    uint8_t * data;
    int datalen;
    volatile int *state;
} periodic_sender_context_t;

void *periodic_sender_routine(void *arg) {
    periodic_sender_context_t *data = (periodic_sender_context_t *)arg;
    while (*(data->state)) {
        sendto(data->sockfd, data->data, data->datalen, 0,
               data->addr, data->addr_len);
        sleep(1);  // Send every 1 second
    }
    free(data);
    return NULL;
}

void periodic_sender(int sockfd, struct sockaddr *addr, socklen_t addr_len, uint8_t* data, int datalen, volatile int *state) {
    pthread_t thread;
    periodic_sender_context_t *sender_data = (periodic_sender_context_t *)malloc(sizeof(periodic_sender_context_t));
    sender_data->sockfd = sockfd;
    sender_data->addr = addr;
    sender_data->addr_len = addr_len;
    sender_data->data = data;
    sender_data->datalen = datalen;
    sender_data->state = state;
    pthread_create(&thread, NULL, periodic_sender_routine, sender_data);
    pthread_detach(thread);  // Detach to run the thread independently
}