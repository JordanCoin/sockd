# Build the router daemon and client
#
# Usage:
#   make          — build both binaries
#   make daemon   — build just the daemon
#   make client   — build just the client
#   make clean    — remove binaries
#   make run      — build and start the daemon
#   make test     — send a PING to a running daemon

CC = cc
CFLAGS = -O2 -Wall -Wextra -pedantic

all: router-daemon router-client bench-harness

router-daemon: router-daemon.c protocol.h
	$(CC) $(CFLAGS) -o $@ router-daemon.c

router-client: router-client.c protocol.h
	$(CC) $(CFLAGS) -o $@ router-client.c

bench-harness: bench-harness.c protocol.h
	$(CC) $(CFLAGS) -o $@ bench-harness.c

clean:
	rm -f router-daemon router-client bench-harness
	rm -f /tmp/ios-skills-router.sock

run: router-daemon
	./router-daemon

test: router-client
	echo '{"tool_name":"Edit","tool_input":{"file_path":"/a/ContentView.swift"},"session_id":"test-1"}' | ./router-client

bench: all
	CLAUDE_PLUGIN_ROOT="$(shell cd ../.. && pwd)" ./bench-harness $(ITERATIONS)

.PHONY: all clean run test
