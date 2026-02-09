CC = gcc
CFLAGS = -O3 -Wall -march=native
LDFLAGS =

TARGETS = trace_controller trace_receiver

all: $(TARGETS)

trace_controller: trace_controller.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

trace_receiver: trace_receiver.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean
