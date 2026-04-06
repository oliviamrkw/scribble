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

# Start the server with verbose (DEBUG) logging
./server -v
./server -v 5000

# Connect a client
./client <server_ip> <port>

# Run game logic tests
make test

# Clean build artifacts
make clean
```

## Server Admin Console

While the server is running, type commands on stdin:

| Command         | Description                          |
|-----------------|--------------------------------------|
| `/status`       | Show uptime, players, game state     |
| `/kick <name>`  | Kick a player by name                |
| `/endround`     | Force-end the current round          |
| `/quit`         | Graceful shutdown                    |
| `/help`         | List available commands              |

## Server Features

- **Structured logging** — timestamped, leveled output (`[HH:MM:SS][LEVEL]`)
- **Duplicate name rejection** — prevents two players from using the same name
- **Idle timeout** — warns at 90 s, kicks at 120 s of inactivity (game only)
- **Progressive hints** — reveals letters at 75 %, 50 %, and 25 % of round time
- **Graceful shutdown** — notifies clients before closing; SIGPIPE safe
- **Admin console** — live server management via stdin (see above)

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
