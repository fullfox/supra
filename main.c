// main.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "network.h"
#include "file_transfer.h"
#include "utils.h"

void print_usage(const char *prog_name) {
    printf("Usage:\n");
    printf("  %s send <file_path> [options]\n", prog_name);
    printf("  %s receive [options]\n", prog_name);
    printf("\nOptions:\n");
    printf("  --packet-size <size>    Specify packet size (default: max UDP size)\n");
    printf("  --dest-ip <ip>          Destination IP address\n");
    printf("  --dest-port <port>      Destination port\n");
    printf("  --help                  Display this help message\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    if (strcmp(argv[1], "send") == 0) {
        // Sender mode
        if (argc < 3) {
            fprintf(stderr, "Error: File path required for sender mode.\n");
            print_usage(argv[0]);
            exit(EXIT_FAILURE);
        }
        const char *file_path = argv[2];
        sender_run(file_path, argc - 3, &argv[3]);
    } else if (strcmp(argv[1], "receive") == 0) {
        // Receiver mode
        receiver_run(argc - 2, &argv[2]);
    } else {
        print_usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    return 0;
}
