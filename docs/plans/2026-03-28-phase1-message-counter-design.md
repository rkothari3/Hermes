# Phase 1 Design: ITCH Message Counter

## Goal
Open a decompressed NASDAQ ITCH 5.0 binary file, read every message using the 2-byte length-prefix framing, count occurrences of each message type, and print a sorted histogram.

## Scope
Single file: `phase1_counter.cpp`. No CMake. Compiles with one command.

## Input
Decompressed ITCH binary (gunzip the .gz first). Path passed as argv[1].

## ITCH Framing
Each message on disk:
  [2-byte big-endian length][message body of that length]
  First byte of body = message type character (ASCII).

## Read Strategy
Use setvbuf to set a 4MB C stdio read buffer. Simple fread loop — no manual chunking needed for Phase 1.

## Output
- Total message count
- Per-type count + percentage, sorted by count descending
- Human-readable type names

## Compile
  g++ -std=c++17 -O2 -Wall -Wextra phase1_counter.cpp -o counter

## Hardware
Intel Core Ultra 7 155H, WSL2, 32GB RAM. Dec 30 2019 ITCH file (~3.3GB gz).
