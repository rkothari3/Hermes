#include <cstdio>
int main(int argc, char* argv[]) {
    if (argc != 2) { fprintf(stderr, "Usage: %s <itch_file>\n", argv[0]); return 1; }
    fprintf(stdout, "Hermes Phase 2 — parser stub. File: %s\n", argv[1]);
    return 0;
}
