// Host tests for the save-area scan in payload/src/repro/repro_scan.c.
// Build: make -C repro_tools/test   Run: ./test_scan [rom_dump.gba]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "repro_flash.h"

#define MB (1024 * 1024)

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

// Writes one save slot (14 sectors) with ids rotated by 'rotation'.
static void make_slot(uint8_t *slot, uint32_t counter, int rotation, int breakLastSignature)
{
    int i;
    for (i = 0; i < 14; i++)
    {
        uint8_t *sector = slot + i * 0x1000;
        int id = (i + 14 - rotation) % 14;
        memset(sector, 0x11 * (id + 1), 0xF80);
        memset(sector + 0xF80, 0, 0x74);
        put16(sector + 0xFF4, id);
        put16(sector + 0xFF6, 0x1234);
        put32(sector + 0xFF8, 0x08012025);
        put32(sector + 0xFFC, counter);
        if (breakLastSignature && id == 13)
            sector[0xFF8] = 0xFF; // phase-1 state of a link save
    }
}

static uint8_t *make_rom(uint32_t size, uint32_t seed)
{
    uint8_t *rom = malloc(REPRO_ROM_MAX_SIZE);
    uint32_t i, x = seed;
    for (i = 0; i < size; i++)
    {
        x = x * 1103515245 + 12345;
        rom[i] = x >> 16;
    }
    memcpy(rom + 0xA0, "POKEMON EMERBPEE01", 18);
    // Readable window is always 32 MiB; smaller chips mirror.
    for (i = size; i < REPRO_ROM_MAX_SIZE; i += size)
        memcpy(rom + i, rom, size);
    return rom;
}

static void test_synthetic(void)
{
    struct ReproScanResult r;
    uint8_t *rom;

    // 16 MiB chip mirrored into 32 MiB, both slots complete at 0xFC0000.
    rom = make_rom(16 * MB, 1);
    memset(rom + 0xFC0000, 0xFF, 0x20000);
    make_slot(rom + 0xFC0000, 41, 3, 0);
    make_slot(rom + 0xFC0000 + 0xE000, 42, 4, 0);
    memcpy(rom + 16 * MB, rom, 16 * MB);
    CHECK(ReproGetRomSize(rom) == 16 * MB, "size %x", ReproGetRomSize(rom));
    CHECK(ReproScanForSave(rom, ReproGetRomSize(rom), &r), "not found");
    CHECK(r.saveOffset == 0xFC0000, "offset %x", r.saveOffset);
    CHECK(r.newestCounter == 42, "counter %u", r.newestCounter);
    CHECK(r.numCandidates == 1, "candidates %u", r.numCandidates);
    CHECK(r.completeSlots == 3, "slots %u", r.completeSlots);
    free(rom);

    // 32 MiB chip, only slot B complete, slot A interrupted mid-save.
    rom = make_rom(32 * MB, 2);
    memset(rom + 0xF80000, 0xFF, 0x20000);
    make_slot(rom + 0xF80000, 8, 0, 1);
    make_slot(rom + 0xF80000 + 0xE000, 7, 13, 0);
    CHECK(ReproGetRomSize(rom) == 32 * MB, "size %x", ReproGetRomSize(rom));
    CHECK(ReproScanForSave(rom, ReproGetRomSize(rom), &r), "not found");
    CHECK(r.saveOffset == 0xF80000, "offset %x", r.saveOffset);
    CHECK(r.newestCounter == 7, "counter %u", r.newestCounter);
    CHECK(r.completeSlots == 2, "slots %u", r.completeSlots);
    free(rom);

    // No save at all.
    rom = make_rom(16 * MB, 3);
    CHECK(!ReproScanForSave(rom, ReproGetRomSize(rom), &r), "found a save in random data");
    free(rom);

    // A save that is not 64 KiB aligned must be ignored rather than misread.
    rom = make_rom(16 * MB, 4);
    make_slot(rom + 0xFC1000, 5, 0, 0);
    make_slot(rom + 0xFC1000 + 0xE000, 6, 0, 0);
    CHECK(!ReproScanForSave(rom, ReproGetRomSize(rom), &r), "accepted a misaligned save at %x", r.saveOffset);
    free(rom);
}

static void test_dump(const char *path)
{
    struct ReproScanResult r;
    uint8_t *rom = calloc(1, REPRO_ROM_MAX_SIZE);
    FILE *f = fopen(path, "rb");
    size_t n, i;
    if (!f) { perror(path); failures++; return; }
    n = fread(rom, 1, REPRO_ROM_MAX_SIZE, f);
    fclose(f);
    // Mirror a short dump the way the cart bus would.
    for (i = n; n && i < REPRO_ROM_MAX_SIZE; i += n)
        memcpy(rom + i, rom, (REPRO_ROM_MAX_SIZE - i < n) ? REPRO_ROM_MAX_SIZE - i : n);
    printf("dump: %zu bytes, readable size 0x%X\n", n, ReproGetRomSize(rom));
    if (ReproScanForSave(rom, ReproGetRomSize(rom), &r))
        printf("dump: save at 0x%X, newest counter %u, slots %u, candidates %u\n",
               r.saveOffset, r.newestCounter, r.completeSlots, r.numCandidates);
    else
        printf("dump: no save found\n");
    free(rom);
}

int main(int argc, char **argv)
{
    test_synthetic();
    if (argc > 1)
        test_dump(argv[1]);
    printf(failures ? "%d FAILURES\n" : "all scan tests passed\n", failures);
    return failures != 0;
}
