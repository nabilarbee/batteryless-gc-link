// Host tests for the repro write path: board probing, the SRAM working copy
// and commits, run against the simulated carts in sim_cart.c.
// The save sequence below follows libpmagb/save.c (sub_0200A2C8 cases 0 and 1).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sim_cart.h"
#include "repro_flash.h"
#include "repro_cart.h"
#include "repro_image.h"

uint16_t HostRomRead16(uint32_t offset);

#define SAVE_OFFSET 0xFC0000
#define SECTOR 0x1000

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("    FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static void make_sector(uint8_t *s, int id, uint32_t counter)
{
    int i;
    for (i = 0; i < 0xF80; i++)
        s[i] = (uint8_t)(i * 7 + id * 13 + counter * 31);
    memset(s + 0xF80, 0, 0x74);
    put16(s + 0xFF4, id);
    put16(s + 0xFF6, 0xBEEF ^ counter);
    put32(s + 0xFF8, 0x08012025);
    put32(s + 0xFFC, counter);
}

// Physical sector of chunk 'id' in a slot, as HandleWriteSector computes it.
static int physical(int slot, int id, int rotation)
{
    return slot * 14 + (id + rotation) % 14;
}

static void write_slot_to_rom(uint8_t *rom, int slot, uint32_t counter, int rotation)
{
    int id;
    for (id = 0; id < 14; id++)
        make_sector(rom + SAVE_OFFSET + physical(slot, id, rotation) * SECTOR, id, counter);
}

// Phase 1 of a link save into 'slot': 13 sectors programmed whole, the last
// one erased and programmed byte by byte except the first signature byte.
static void phase1(int slot, uint32_t counter, int rotation, int *lastSector)
{
    uint8_t buf[SECTOR];
    int id, i;

    for (id = 0; id < 13; id++)
    {
        int sector = physical(slot, id, rotation);
        make_sector(buf, id, counter);
        ReproImage_Write(sector * SECTOR, buf, SECTOR);
        CHECK(ReproImage_Compare(sector * SECTOR, buf, SECTOR) == 0, "verify of sector %d failed", sector);
    }

    *lastSector = physical(slot, 13, rotation);
    make_sector(buf, 13, counter);
    ReproImage_Fill(*lastSector * SECTOR, 0xFF, SECTOR);
    for (i = 0; i < SECTOR; i++)
    {
        if (i != 0xFF8)
            ReproImage_Write(*lastSector * SECTOR + i, &buf[i], 1);
    }
}

static void phase2(int lastSector)
{
    uint8_t sig = 0x25;
    ReproImage_Write(lastSector * SECTOR + 0xFF8, &sig, 1);
}

static int rom_matches_image(const uint8_t *rom, const uint8_t *expected, uint32_t *where)
{
    uint32_t i;
    for (i = 0; i < REPRO_SAVE_SIZE; i++)
    {
        if (rom[SAVE_OFFSET + i] != expected[i])
        {
            *where = i;
            return 0;
        }
    }
    return 1;
}

static const char *sBoardNames[] = {"plain ROM", "Intel buffered", "Intel word", "AMD swapped", "AMD dual x8", "AMD x16", "Intel, unknown ID"};

static void test_board(enum SimBoard board, int expectedType, int expectSupported)
{
    uint8_t *rom;
    uint8_t *before = malloc(SIM_ROM_SIZE);
    uint8_t expected[REPRO_SAVE_SIZE];
    struct ReproScanResult scan;
    const struct ReproCart *cart;
    uint32_t where;
    int lastSector;
    u16 result;
    unsigned erasesBefore;

    printf("  %s\n", sBoardNames[board]);
    SimInit(board, 1234 + board);
    rom = SimRom();

    // Existing save: slot A counter 10 (rotation 3), slot B counter 9 (rotation 2),
    // sectors 28-31 hold other data.
    memset(rom + SAVE_OFFSET, 0xFF, REPRO_SAVE_SIZE);
    write_slot_to_rom(rom, 0, 10, 3);
    write_slot_to_rom(rom, 1, 9, 2);
    make_sector(rom + SAVE_OFFSET + 28 * SECTOR, 0, 77);
    make_sector(rom + SAVE_OFFSET + 29 * SECTOR, 1, 77);
    memcpy(before, rom, SIM_ROM_SIZE);

    CHECK(ReproScanForSave(rom, ReproGetRomSize(rom), &scan) && scan.saveOffset == SAVE_OFFSET, "scan");

    cart = ReproCart_Probe();
    CHECK(cart->type == expectedType, "probe gave type %d, expected %d", cart->type, expectedType);
    CHECK(cart->supported == expectSupported, "supported flag");
    CHECK(memcmp(rom, before, SIM_ROM_SIZE) == 0, "probe changed ROM contents");
    CHECK(HostRomRead16(0) == 0x002E && HostRomRead16(2) == 0xEA00, "chip not back in read-array mode after the probe");
    if (!cart->supported)
    {
        CHECK(ReproImage_Commit(cart, SAVE_OFFSET) == REPRO_ERR_UNSUPPORTED, "commit on unsupported board");
        free(before);
        return;
    }

    CHECK(ReproImage_Load(SAVE_OFFSET) == REPRO_OK, "load");
    CHECK(memcmp(SimSram(), rom + SAVE_OFFSET, REPRO_SAVE_SIZE) == 0, "SRAM copy differs from ROM");

    // Nothing changed: commit must not touch the chip.
    erasesBefore = SimStats()->erases;
    CHECK(ReproImage_Commit(cart, SAVE_OFFSET) == REPRO_OK, "empty commit");
    CHECK(SimStats()->erases == erasesBefore && SimStats()->programs == 0, "empty commit wrote to flash");

    // Save 1: counter 11 goes to slot B (11 % 2), rotation (2+1) % 14.
    phase1(1, 11, 3, &lastSector);
    result = ReproImage_Commit(cart, SAVE_OFFSET);
    CHECK(result == REPRO_OK, "phase 1 commit returned 0x%X", result);
    memcpy(expected, SimSram(), REPRO_SAVE_SIZE);
    // ROM must match the image except around the pending signature, which stays erased.
    {
        uint32_t unit = cart->programUnit;
        uint32_t sig = lastSector * SECTOR + 0xFF8;
        uint32_t start = sig & ~(unit - 1);
        memset(expected + start, 0xFF, unit);
    }
    CHECK(rom_matches_image(rom, expected, &where), "after phase 1 ROM differs at save+0x%X", where);
    CHECK(ReproScanForSave(rom, ReproGetRomSize(rom), &scan) && scan.newestCounter == 10, "after phase 1 the newest valid save must still be 10 (got %u)", scan.newestCounter);
    CHECK(memcmp(rom, before, SAVE_OFFSET) == 0 && memcmp(rom + SAVE_OFFSET + REPRO_SAVE_SIZE, before + SAVE_OFFSET + REPRO_SAVE_SIZE, SIM_ROM_SIZE - SAVE_OFFSET - REPRO_SAVE_SIZE) == 0, "phase 1 changed ROM outside the save area");

    erasesBefore = SimStats()->erases;
    phase2(lastSector);
    result = ReproImage_Commit(cart, SAVE_OFFSET);
    CHECK(result == REPRO_OK, "phase 2 commit returned 0x%X", result);
    CHECK(SimStats()->erases == erasesBefore, "phase 2 erased flash");
    CHECK(rom_matches_image(rom, SimSram(), &where), "after phase 2 ROM differs at save+0x%X", where);
    CHECK(ReproScanForSave(rom, ReproGetRomSize(rom), &scan) && scan.newestCounter == 11 && scan.completeSlots == 3, "after phase 2: counter %u slots %u", scan.newestCounter, scan.completeSlots);

    // Save 2: counter 12 goes back to slot A, which already holds data.
    phase1(0, 12, 4, &lastSector);
    CHECK(ReproImage_Commit(cart, SAVE_OFFSET) == REPRO_OK, "second phase 1 commit");
    CHECK(ReproScanForSave(rom, ReproGetRomSize(rom), &scan) && scan.newestCounter == 11, "second phase 1: newest %u", scan.newestCounter);
    phase2(lastSector);
    CHECK(ReproImage_Commit(cart, SAVE_OFFSET) == REPRO_OK, "second phase 2 commit");
    CHECK(rom_matches_image(rom, SimSram(), &where), "after second save ROM differs at save+0x%X", where);
    CHECK(ReproScanForSave(rom, ReproGetRomSize(rom), &scan) && scan.newestCounter == 12 && scan.completeSlots == 3, "after second save: counter %u slots %u", scan.newestCounter, scan.completeSlots);
    CHECK(memcmp(rom, before, SAVE_OFFSET) == 0 && memcmp(rom + SAVE_OFFSET + REPRO_SAVE_SIZE, before + SAVE_OFFSET + REPRO_SAVE_SIZE, SIM_ROM_SIZE - SAVE_OFFSET - REPRO_SAVE_SIZE) == 0, "ROM changed outside the save area");

    // A failed erase is retried once.
    phase1(1, 13, 5, &lastSector);
    SimFailNextErase();
    result = ReproImage_Commit(cart, SAVE_OFFSET);
    CHECK(result == REPRO_OK, "commit after one failed erase returned 0x%X", result);
    phase2(lastSector);
    CHECK(ReproImage_Commit(cart, SAVE_OFFSET) == REPRO_OK, "phase 2 after retry");
    CHECK(ReproScanForSave(rom, ReproGetRomSize(rom), &scan) && scan.newestCounter == 13, "after retry: newest %u", scan.newestCounter);

    // A failed word program is reported (or retried) instead of hanging.
    phase1(0, 14, 6, &lastSector);
    SimFailNextProgram();
    result = ReproImage_Commit(cart, SAVE_OFFSET);
    CHECK(result == REPRO_OK, "commit after one failed program returned 0x%X", result);
    phase2(lastSector);
    CHECK(ReproImage_Commit(cart, SAVE_OFFSET) == REPRO_OK, "phase 2 after program failure");
    CHECK(ReproScanForSave(rom, ReproGetRomSize(rom), &scan) && scan.newestCounter == 14, "after program retry: newest %u", scan.newestCounter);

    CHECK(SimStats()->violations == 0, "%u chip protocol violations, last: %s", SimStats()->violations, SimLastViolation());
    CHECK(SimStats()->badBankWrites == 0, "%u bank register writes with values this board does not use", SimStats()->badBankWrites);
    printf("    erases %u, program operations %u, stray writes %u\n", SimStats()->erases, SimStats()->programs, SimStats()->strayWrites);
    free(before);
}

int main(void)
{
    test_board(SIM_PLAIN_ROM, REPRO_CART_UNKNOWN, 0);
    test_board(SIM_AMD_SWAPPED, REPRO_CART_AMD_SWAPPED, 1);
    test_board(SIM_AMD_DUAL, REPRO_CART_AMD_DUAL, 1);
    test_board(SIM_AMD_X16, REPRO_CART_AMD_X16, 0);
    test_board(SIM_INTEL_BUFFERED, REPRO_CART_INTEL_BUFFERED, 1);
    test_board(SIM_INTEL_WORD, REPRO_CART_INTEL_WORD, 1);
    test_board(SIM_INTEL_UNKNOWN, REPRO_CART_UNKNOWN, 0);
    printf(failures ? "%d FAILURES\n" : "all write tests passed\n", failures);
    return failures != 0;
}
