// Phase 1: NASDAQ ITCH 5.0 Message Counter
// Reads a decompressed ITCH binary file and counts messages by type.
//
// Compile: g++ -std=c++17 -O2 -Wall -Wextra phase1_counter.cpp -o counter
// Run:     ./counter /path/to/decompressed.NASDAQ_ITCH50
//
// ITCH framing: every message is prefixed by a 2-byte big-endian length.
// The length tells us how many bytes the message body contains (not counting
// the 2 prefix bytes themselves). The first byte of the body is the message
// type character.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <algorithm>

// Human-readable names for every ITCH 5.0 message type.
// Source: NASDAQ TotalView-ITCH 5.0 specification.
static const char* msg_type_name(uint8_t type) {
    switch (type) {
        case 'S': return "System Event";
        case 'R': return "Stock Directory";
        case 'H': return "Stock Trading Action";
        case 'Y': return "Reg SHO Short Sale Price Test Restriction";
        case 'L': return "Market Participant Position";
        case 'V': return "MWCB Decline Level";
        case 'W': return "MWCB Status";
        case 'K': return "IPO Quoting Period Update";
        case 'J': return "LULD Auction Collar";
        case 'h': return "Operational Halt";
        case 'A': return "Add Order (no MPID)";
        case 'F': return "Add Order (with MPID)";
        case 'E': return "Order Executed";
        case 'C': return "Order Executed with Price";
        case 'X': return "Order Cancel";
        case 'D': return "Order Delete";
        case 'U': return "Order Replace";
        case 'P': return "Trade (non-displayable)";
        case 'Q': return "Cross Trade";
        case 'B': return "Broken Trade / Order";
        case 'I': return "Net Order Imbalance Indicator (NOII)";
        case 'N': return "Retail Price Improvement Indicator";
        default:  return "Unknown";
    }
}

// Format a uint64_t with comma separators, e.g. 1234567 -> "1,234,567"
static void format_commas(uint64_t n, char* out, size_t out_size) {
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)n);
    int len = (int)strlen(tmp);
    int commas = (len - 1) / 3;
    int total = len + commas;
    if ((size_t)total + 1 > out_size) { snprintf(out, out_size, "%s", tmp); return; }
    out[total] = '\0';
    int src = len - 1;
    int dst = total - 1;
    int count = 0;
    while (src >= 0) {
        if (count > 0 && count % 3 == 0) out[dst--] = ',';
        out[dst--] = tmp[src--];
        count++;
    }
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <decompressed_itch_file>\n", argv[0]);
        fprintf(stderr, "  First gunzip your .gz file, then pass the raw binary.\n");
        return 1;
    }

    FILE* f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open '%s'\n", argv[1]);
        return 1;
    }

    // Tell the C library to buffer 4MB at a time.
    // Without this, the default buffer is ~8KB — too many small OS reads.
    static char io_buf[4 * 1024 * 1024];
    setvbuf(f, io_buf, _IOFBF, sizeof(io_buf));

    // counts[x] = number of messages whose first body byte equals x.
    // Indexed directly by ASCII value — O(1) lookup, zero overhead.
    uint64_t counts[256] = {};
    uint64_t total = 0;

    // Reusable scratch buffer for message bodies.
    // Max ITCH message is a few hundred bytes; 65535 is more than enough.
    uint8_t body[65535];

    while (true) {
        // --- Read the 2-byte big-endian length prefix ---
        uint8_t len_bytes[2];
        if (fread(len_bytes, 1, 2, f) != 2) break;  // EOF or error

        // Manually reconstruct as big-endian.
        // DO NOT cast raw bytes to uint16_t — that would be little-endian on x86.
        uint16_t msg_len = (uint16_t)((len_bytes[0] << 8) | len_bytes[1]);

        if (msg_len == 0) continue;  // shouldn't happen, but be safe

        // --- Read the message body ---
        if (fread(body, 1, msg_len, f) != (size_t)msg_len) break;

        // --- Count by type (first byte of body) ---
        counts[body[0]]++;
        total++;

        // Progress indicator: print a dot every 10 million messages
        // so you know it's running on the 10GB file.
        if (total % 10'000'000 == 0) {
            fprintf(stderr, "  ... %llu million messages processed\r",
                    (unsigned long long)(total / 1'000'000));
            fflush(stderr);
        }
    }

    fclose(f);
    fprintf(stderr, "\n");  // clear the progress line

    // --- Print results ---
    char buf[32];
    format_commas(total, buf, sizeof(buf));
    printf("\nTotal messages: %s\n\n", buf);
    printf("%-6s  %-42s  %18s  %7s\n", "Type", "Description", "Count", "% Total");
    printf("%-6s  %-42s  %18s  %7s\n",
           "------", "------------------------------------------",
           "------------------", "-------");

    // Collect non-zero entries and sort by count descending
    struct Entry { uint8_t type; uint64_t count; };
    Entry entries[256];
    int n = 0;
    for (int i = 0; i < 256; i++) {
        if (counts[i] > 0) {
            entries[n++] = { (uint8_t)i, counts[i] };
        }
    }
    std::sort(entries, entries + n, [](const Entry& a, const Entry& b) {
        return a.count > b.count;
    });

    for (int i = 0; i < n; i++) {
        char count_buf[32];
        format_commas(entries[i].count, count_buf, sizeof(count_buf));
        printf("  '%c'   %-42s  %18s  %6.2f%%\n",
               (char)entries[i].type,
               msg_type_name(entries[i].type),
               count_buf,
               100.0 * (double)entries[i].count / (double)total);
    }

    return 0;
}
