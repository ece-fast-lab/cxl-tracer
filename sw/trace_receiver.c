#include <arpa/inet.h>
#include <emmintrin.h>
#include <fcntl.h>
#include <immintrin.h>
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
#define TRACE_BUFFER_SIZE (10UL * 1024 * 1024 * 1024)
#define DEFAULT_TRACE_FILE "/tmp/trace_records.bin"
#define COMM_PORT 8888
#define DAX_PATH "/dev/dax0.0"

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
volatile sig_atomic_t trace_running = 0;

void signal_handler() { keep_running = 0; }

uint64_t get_pfn(void *virt_addr);
uint64_t virt_to_phys(void *virt_addr);

int setup_server_socket() {
  int sockfd;
  struct sockaddr_in serv_addr;
  int opt = 1;

  sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    perror("socket creation failed");
    return -1;
  }

  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    perror("setsockopt failed");
    close(sockfd);
    return -1;
  }

  memset(&serv_addr, 0, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = INADDR_ANY;
  serv_addr.sin_port = htons(COMM_PORT);

  if (bind(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
    perror("bind failed");
    close(sockfd);
    return -1;
  }

  if (listen(sockfd, 1) < 0) {
    perror("listen failed");
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
  if (argc < 1 || argc > 3) {
    fprintf(stderr, "Usage: %s [output_file] [buffer_size_gb]\n", argv[0]);
    fprintf(stderr, "Example: %s /path/to/trace.bin 1\n", argv[0]);
    fprintf(stderr,
            "  - buffer_size_gb: Size of trace buffer in GB (default: 10GB)\n");
    return EXIT_FAILURE;
  }

  const char *output_file = DEFAULT_TRACE_FILE;
  size_t buffer_size = TRACE_BUFFER_SIZE;

  // Parse output file path if provided
  if (argc >= 2) {
    output_file = argv[1];
  }

  // Parse buffer size if provided (in GB)
  if (argc >= 3) {
    char *endptr;
    long buffer_size_gb = strtol(argv[2], &endptr, 10);
    if (*endptr != '\0' || buffer_size_gb <= 0) {
      fprintf(stderr,
              "Invalid buffer size: %s. Must be a positive number in GB.\n",
              argv[2]);
      return EXIT_FAILURE;
    }
    buffer_size = buffer_size_gb * 1024UL * 1024UL * 1024UL;
  }

  printf("Output file: %s\n", output_file);
  printf("Buffer size: %zu bytes (%.2f GB)\n", buffer_size,
         buffer_size / (1024.0 * 1024.0 * 1024.0));

  // Set up signal handler for Ctrl+C
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, NULL);

  // Open /dev/dax to map physical memory
  int mem_fd = open(DAX_PATH, O_RDWR | O_SYNC);
  if (mem_fd == -1) {
    perror("Failed to open /dev/dax");
    fprintf(stderr, "Did you run this program as root (e.g., using sudo)?\n");
    return EXIT_FAILURE;
  }

  // Map memory region for trace buffer from physical memory
  void *mem = mmap(NULL, buffer_size, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_POPULATE, mem_fd, 0);
  if (mem == MAP_FAILED) {
    perror("mmap for trace buffer");
    close(mem_fd);
    return EXIT_FAILURE;
  }

  printf("Mapped trace buffer at virtual address: %p\n", mem);

  // Get physical address of the buffer
  uint64_t trace_buffer_phys_addr = virt_to_phys(mem);
  printf("Trace buffer physical address: 0x%016lx\n", trace_buffer_phys_addr);

  // Clear the buffer
  printf("Clearing trace buffer...\n");
  memset(mem, 0, buffer_size);
  _mm_sfence();

  // Flush the entire buffer from cache
  printf("Flushing buffer from cache...\n");
  size_t i = 0;
  size_t flush_unroll_limit = buffer_size & ~511UL;
  for (; i < flush_unroll_limit; i += 512) {
    _mm_clflushopt(mem + i);
    _mm_clflushopt(mem + i + 64);
    _mm_clflushopt(mem + i + 128);
    _mm_clflushopt(mem + i + 192);
    _mm_clflushopt(mem + i + 256);
    _mm_clflushopt(mem + i + 320);
    _mm_clflushopt(mem + i + 384);
    _mm_clflushopt(mem + i + 448);
  }

  // Handle remaining cache lines
  for (; i < buffer_size; i += 64) {
    _mm_clflushopt(mem + i);
  }
  _mm_mfence();

  // Set up server socket
  printf("Setting up server socket on port %d...\n", COMM_PORT);
  int server_fd = setup_server_socket();
  if (server_fd < 0) {
    fprintf(stderr, "Failed to setup server socket\n");
    goto cleanup_mem;
  }

  printf("Waiting for connection from trace controller...\n");
  struct sockaddr_in client_addr;
  socklen_t client_len = sizeof(client_addr);
  int client_fd =
      accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
  if (client_fd < 0) {
    perror("accept failed");
    goto cleanup_server;
  }

  printf("Connected to controller at %s\n", inet_ntoa(client_addr.sin_addr));

  // Send buffer info to controller
  comm_msg_t msg;
  msg.msg_type = MSG_BUFFER_INFO;
  msg.buffer_addr = trace_buffer_phys_addr;
  msg.buffer_size = buffer_size;
  if (!send_message(client_fd, &msg)) {
    fprintf(stderr, "Failed to send buffer info to controller\n");
    goto cleanup_client;
  }

  printf("Sent buffer info to controller\n");
  printf("Buffer ready. Waiting for trace commands...\n");

  // Main communication loop
  while (keep_running) {
    fd_set read_fds;
    struct timeval timeout;

    FD_ZERO(&read_fds);
    FD_SET(client_fd, &read_fds);

    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    int activity = select(client_fd + 1, &read_fds, NULL, NULL, &timeout);

    if (activity < 0) {
      perror("select failed");
      break;
    }

    if (activity == 0) {
      // Timeout, continue loop
      continue;
    }

    if (FD_ISSET(client_fd, &read_fds)) {
      if (!recv_message(client_fd, &msg)) {
        printf("Controller disconnected\n");
        break;
      }

      switch (msg.msg_type) {
      case MSG_START_TRACE:
        printf("Received start trace command\n");
        trace_running = 1;
        break;

      case MSG_STOP_TRACE:
        printf("Received stop trace command\n");
        trace_running = 0;

        uint64_t dropped_traces = msg.dropped_traces;
        uint64_t written_traces = msg.written_traces;

        printf("Trace statistics:\n");
        printf("  Dropped traces: %lu\n", dropped_traces);
        printf("  Written traces: %lu\n", written_traces);

        const size_t skip_count = 0;
        written_traces = written_traces - skip_count;

        // Save trace data to file
        printf("Saving trace data to %s...\n", output_file);

        // Define header structure for the binary file
        struct {
          uint32_t magic;
          uint32_t version;
          uint64_t buffer_size;
          uint64_t written_traces;
          uint64_t dropped_traces;
        } file_header = {.magic = 0x54524143, // "TRAC" in ASCII
                         .version = 1,
                         .buffer_size = buffer_size,
                         .written_traces = written_traces,
                         .dropped_traces = dropped_traces};

        // Create output file
        int out_fd = open(output_file, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (out_fd == -1) {
          perror("Failed to open output file");
          break;
        }

        posix_fadvise(out_fd, 0, 0, POSIX_FADV_SEQUENTIAL);

        // Write header
        if (write(out_fd, &file_header, sizeof(file_header)) !=
            sizeof(file_header)) {
          perror("Failed to write file header");
          close(out_fd);
          break;
        }

        // Write trace data
        size_t record_size = 16;
        size_t bytes_to_write = written_traces * record_size;
        size_t bytes_written = 0;
        char *write_ptr = (char *)mem + (skip_count * record_size);

        while (bytes_written < bytes_to_write) {
          ssize_t result = write(out_fd, write_ptr + bytes_written,
                                 bytes_to_write - bytes_written);
          if (result < 0) {
            perror("Failed to write trace buffer");
            break;
          }
          bytes_written += result;
        }

        close(out_fd);

        printf("Saved trace buffer to %s\n", output_file);
        printf("File size: %zu bytes (header: %zu bytes, data: %zu bytes)\n",
               sizeof(file_header) + bytes_written, sizeof(file_header),
               bytes_written);

        // Display sample trace records
        printf("\nTrace Records (Sample):\n");
        printf("%-6s %-18s %-18s\n", "Type", "Address", "Timestamp");
        printf("------------------------------------------------\n");

        size_t displayed = 0;
        const size_t max_display = 5;

        for (size_t i = 0; i < buffer_size / 16 && displayed < max_display;
             i++) {
          size_t cache_line_idx = i / 4;
          size_t record_idx = i % 4;

          uint64_t record_low =
              ((uint64_t *)mem)[cache_line_idx * 8 + record_idx * 2];
          uint64_t record_high =
              ((uint64_t *)mem)[cache_line_idx * 8 + record_idx * 2 + 1];

          int valid = (record_low >> 63) & 0x1;
          if (valid) {
            int op_type = (record_low >> 62) & 0x1;
            uint64_t address = record_low & 0x000fffffffffffff;
            uint64_t timestamp = record_high;

            printf("%-6s 0x%016lx 0x%016lx\n", op_type ? "WRITE" : "READ",
                   address, timestamp);
            displayed++;
          }
        }

        // Send acknowledgment
        msg.msg_type = MSG_ACK;
        if (!send_message(client_fd, &msg)) {
          fprintf(stderr, "Failed to send acknowledgment\n");
        }

        printf("Trace processing completed\n");
        break;

      default:
        printf("Received unknown message type: %u\n", msg.msg_type);
        break;
      }
    }
  }

cleanup_client:
  close(client_fd);
cleanup_server:
  close(server_fd);
cleanup_mem:
  if (munmap(mem, buffer_size) == -1) {
    perror("munmap for trace buffer");
  }
  close(mem_fd);

  return EXIT_SUCCESS;
}

uint64_t get_pfn(void *virt_addr) {
  const int page_size = getpagesize();
  int fd = open("/proc/self/pagemap", O_RDONLY);
  uint64_t entry = 0;

  off_t offset = ((uintptr_t)virt_addr / page_size) * sizeof(entry);
  lseek(fd, offset, SEEK_SET);
  if (read(fd, &entry, sizeof(entry)) != sizeof(entry)) {
    perror("Failed to read from /proc/self/pagemap");
    close(fd);
    return -1;
  }
  close(fd);

  return entry & ((1ULL << 55) - 1); // Extract PFN
}

uint64_t virt_to_phys(void *virt_addr) {
  uint64_t pfn = get_pfn(virt_addr);
  if (pfn == 0) {
    fprintf(stderr, "Failed to get PFN for address %p\n", virt_addr);
    return 0;
  }
  uint64_t page_offset = (uintptr_t)virt_addr % getpagesize();
  return (pfn * getpagesize()) + page_offset;
}
