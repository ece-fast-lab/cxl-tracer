#include <arpa/inet.h>
#include <emmintrin.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

// Configuration macros
#define PCI_BAR_REGION_SIZE 0x1000
#define COMM_PORT 8888
#define BUFFER_SIZE 1024

// CSR offsets
#define CSR_BUFFER_ADDR 0x8
#define CSR_BUFFER_SIZE 0x10
#define CSR_TRACE_CONTROL 0x18
#define CSR_DROPPED_TRACES 0x20
#define CSR_WRITTEN_TRACES 0x28

// Communication protocol messages
typedef struct {
    uint32_t msg_type;
    uint64_t buffer_addr;
    uint64_t buffer_size;
    uint64_t dropped_traces;
    uint64_t written_traces;
} comm_msg_t;

#define MSG_BUFFER_INFO 1
#define MSG_START_TRACE 2
#define MSG_STOP_TRACE 3
#define MSG_TRACE_STATS 4
#define MSG_ACK 5

volatile sig_atomic_t keep_running = 1;

void signal_handler() { keep_running = 0; }

int connect_to_receiver(const char *receiver_ip) {
    int sockfd;
    struct sockaddr_in serv_addr;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket creation failed");
        return -1;
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(COMM_PORT);

    if (inet_pton(AF_INET, receiver_ip, &serv_addr.sin_addr) <= 0) {
        perror("inet_pton failed");
        close(sockfd);
        return -1;
    }

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("connection failed");
        close(sockfd);
        return -1;
    }

    return sockfd;
}

int send_message(int sockfd, comm_msg_t *msg) {
    return send(sockfd, msg, sizeof(comm_msg_t), 0) == sizeof(comm_msg_t);
}

int recv_message(int sockfd, comm_msg_t *msg) {
    return recv(sockfd, msg, sizeof(comm_msg_t), 0) == sizeof(comm_msg_t);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <pci_device> <receiver_ip>\n", argv[0]);
        fprintf(stderr, "Example: %s 0000:01:00.0 192.168.1.100\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *pci_device = argv[1];
    const char *receiver_ip = argv[2];
    const int bar_number = 2;

    printf("Connecting to trace receiver at %s...\n", receiver_ip);
    int sockfd = connect_to_receiver(receiver_ip);
    if (sockfd < 0) {
        fprintf(stderr, "Failed to connect to receiver\n");
        return EXIT_FAILURE;
    }
    printf("Connected to receiver\n");

    // Wait for buffer info from receiver
    comm_msg_t msg;
    if (!recv_message(sockfd, &msg) || msg.msg_type != MSG_BUFFER_INFO) {
        fprintf(stderr, "Failed to receive buffer info from receiver\n");
        close(sockfd);
        return EXIT_FAILURE;
    }

    uint64_t trace_buffer_phys_addr = msg.buffer_addr;
    uint64_t buffer_size = msg.buffer_size;

    printf("Received buffer info: addr=0x%016lx, size=%lu bytes (%.2f GB)\n",
           trace_buffer_phys_addr, buffer_size,
           buffer_size / (1024.0 * 1024.0 * 1024.0));

    // Open PCI BAR region
    char bar_path[256];
    snprintf(bar_path, sizeof(bar_path), "/sys/bus/pci/devices/%s/resource%d",
             pci_device, bar_number);

    int fd = open(bar_path, O_RDWR | O_SYNC);
    if (fd == -1) {
        perror("open PCI BAR");
        close(sockfd);
        return EXIT_FAILURE;
    }

    void *bar_addr = mmap(NULL, PCI_BAR_REGION_SIZE, PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd, 0);
    if (bar_addr == MAP_FAILED) {
        perror("mmap for PCI BAR");
        close(fd);
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("Mapped PCI BAR region at address %p\n", bar_addr);

    volatile uint64_t *bar = (volatile uint64_t *)bar_addr;

    // Configure trace buffer
    _mm_mfence();
    bar[CSR_BUFFER_ADDR / sizeof(uint64_t)] = trace_buffer_phys_addr;
    bar[CSR_BUFFER_SIZE / sizeof(uint64_t)] = buffer_size;
    _mm_mfence();
    printf("Configured FPGA with buffer address 0x%016lx, size %lu bytes\n",
           trace_buffer_phys_addr, buffer_size);

    // Set up signal handler for Ctrl+C
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    // Wait for user input before starting trace collection
    printf("\nSetup complete. Press enter to begin trace collection...");
    getchar();

    printf("\nStarting trace collection...\n");

    // Send start message to receiver
    msg.msg_type = MSG_START_TRACE;
    if (!send_message(sockfd, &msg)) {
        fprintf(stderr, "Failed to send start message to receiver\n");
        goto cleanup;
    }

    // Reset and start trace collection on FPGA
    bar[CSR_TRACE_CONTROL / sizeof(uint64_t)] = 4;
    _mm_mfence();
    bar[CSR_TRACE_CONTROL / sizeof(uint64_t)] = 1;
    _mm_mfence();

    printf("Trace collection is running...\n");
    printf("Press Ctrl+C to stop collection\n");

    // Wait until user presses Ctrl+C
    while (keep_running) {
        sleep(1);
    }

    printf("\nReceived termination signal. Stopping trace collection...\n");

    // Stop trace collection on FPGA
    _mm_mfence();
    bar[CSR_TRACE_CONTROL / sizeof(uint64_t)] = 0;
    _mm_mfence();
    printf("Stopped trace collection\n");

    // Read counter values from FPGA
    _mm_mfence();
    uint64_t dropped_traces = bar[CSR_DROPPED_TRACES / sizeof(uint64_t)];
    _mm_mfence();
    uint64_t written_traces = bar[CSR_WRITTEN_TRACES / sizeof(uint64_t)];

    printf("Dropped traces: %lu\nWritten traces: %lu\n", dropped_traces,
           written_traces);

    // Send stop message with statistics to receiver
    msg.msg_type = MSG_STOP_TRACE;
    msg.dropped_traces = dropped_traces;
    msg.written_traces = written_traces;
    if (!send_message(sockfd, &msg)) {
        fprintf(stderr, "Failed to send stop message to receiver\n");
        goto cleanup;
    }

    // Wait for acknowledgment from receiver
    if (!recv_message(sockfd, &msg) || msg.msg_type != MSG_ACK) {
        fprintf(stderr, "Failed to receive acknowledgment from receiver\n");
        goto cleanup;
    }

    printf("Trace collection completed successfully\n");

cleanup:
    // Clean up
    if (munmap(bar_addr, PCI_BAR_REGION_SIZE) == -1) {
        perror("munmap for PCI BAR");
    }
    close(fd);
    close(sockfd);

    return EXIT_SUCCESS;
}