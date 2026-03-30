# Scribble

CSC209 Assignment 3 — Multiplayer Drawing-Guessing Game (Skribbl.io clone)

## Quick Start

```bash
# Build everything
make

# Build with a custom port
make PORT=5000

# Start the server (default port 4242)
./server

# Start the server on a custom port
./server 5000

# Connect a client (once implemented)
./client <server_ip> <port>

# Clean build artifacts
make clean
```

## Project Structure

| File       | Owner    | Description                                       |
|------------|----------|---------------------------------------------------|
| net.h      | Shared   | Message protocol definitions and constants        |
| net.c      | Shared   | Network I/O utilities (send/recv with byte order) |
| server.c   | Person A | Multi-client server with select() multiplexing    |
| client.c   | Person B | Player client with select() on stdin + socket     |
| game.c/h   | Person B | Game logic, scoring, round management             |
| words.txt  | Person B | Word bank (one word per line)                     |
| Makefile   | Shared   | Build automation                                  |

## Requirements

- GCC with C99 support
- POSIX environment (Linux / teach.cs)
- No external libraries
