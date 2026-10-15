# How it works

## 1. The cart

A genuine Pokémon Emerald cart has a 16 MB mask ROM, a 128 KB flash chip for
the save (at the GBA's save address, 0x0E000000) and a battery-powered clock.

A batteryless repro has one NOR flash chip for everything and no battery. On
the tested cart:

```
0x0000000  Emerald (entry branch patched to the batteryless code)
0x0E3CF64  free space (0xFF)
0x0FA0000  the bootleg batteryless code
0x0FC0000  the save: a standard 128 KB Gen 3 save image (32 sectors of 4 KB)
0x1000000  rest of the chip (the GBA sees 32 MB of a 64 MB chip)
```

At the save address there is 128 KB of SRAM in two 64 KB banks, selected by a
byte written to 0x09000000. It holds a working copy of the save, but loses it
at power-off. At power-on, the batteryless code copies the save from ROM into
SRAM. Emerald's flash save routines are patched to use that SRAM, and after a
save the batteryless code erases the ROM save area and programs it again from
SRAM.

The flash chip is one of a few board types known from the bootleg patch (see
`src/repro/repro_cart.c`): Intel with buffered or word programming, AMD x16
with data lines D0 and D1 swapped (the tested cart), or two AMD x8 chips side
by side. Each needs its own command sequences, unlock addresses and status
polling.

## 2. Why the GameCube games fail

Colosseum and XD send GBA programs over the link cable, which the GBA runs
from RAM while the game cart is inserted. These programs carry Nintendo's
flash library. To read or write the save, they first identify the flash chip
with ID commands sent to 0x0E005555/0x0E002AAA.

On a repro, those writes land in plain SRAM, no chip answers, and the program
reports that there is no save, or that it was deleted. Even if it found the
save, it would write it with the commands of a flash chip that isn't there.

## 3. The save backend (`src/repro`)

All the fixes share one backend that serves the flash library's interface
from the cart's ROM:

- **Finding the save.** It scans cart ROM in 64 KB steps for a 128 KB area
  of 4 KB sectors with the Gen 3 signature (0x08012025) at +0xFF8 and valid
  checksums. A genuine cart has none, and the original flash code runs as
  before.
- **Reading.** Reads come straight from ROM.
- **First write.** It probes the chip (ID and CFI) to find the board type,
  then copies the save area into the SRAM working copy. From then on every
  write goes to that copy.
- **Commit.** It compares the copy with ROM in program units:
  - If only erased units need data, it programs them.
  - Otherwise it erases the save area's blocks and programs everything
    again, newest save slot first, so a valid save is back in ROM as early
    as possible.
  - Everything is verified, and retried once.
- **Two-phase link saves.** Gen 3 link saves are written in two phases: the
  whole slot except one signature byte, then that byte. The commit leaves
  the unit holding that byte erased after the first phase. The second phase
  then only programs, which is fast, and an interrupted trade leaves the
  previous slot valid.
- **Timing.** Each chip operation runs with interrupts off, from RAM.

## 4. Colosseum

On the copyright screen, Emerald listens for a GameCube and receives a
program. If the program's header has Colosseum's game code ("Gc6e"), Emerald
discards it and runs its own copy from ROM (0x28000 bytes at 0x9AA144),
which knows Emerald's save layout.

[pret/colosseum-mb](https://github.com/pret/colosseum-mb) decompiles that
program. With `REPRO=1` (the patches in `patches/colosseum-mb`), the backend
is linked in:

- `IdentifyFlash` tries the backend first;
- `ReadFlash` and `VerifyFlashSector` use it once it is active;
- the link code commits after each of the two save phases, before the result
  goes back to the GameCube.

The new program is shipped as a BPS patch from Emerald's copy, so it can be
applied to the user's own dump. The flasher writes it over Emerald's copy.

## 5. XD

XD sends its own programs, one for trading and one for Battle Mode with the
GBA, in several languages. Emerald runs them as sent. Each program is:

```
0x000  header and a small stub
0x3E0  the real program, LZ77-compressed
end    a loader, copied to IWRAM by the stub: it moves the compressed data to
       the top of EWRAM, decompresses it to 0x02000000 and jumps there
```

When it sends a program, XD writes its own value over the header's game code
("TEST" in the file on the disc). The loader preserves that value through
decompression, and the program checks it.

### Finding what to change (`src/xdlink/analyze_xd.py`)

- **The flash library.** It is byte-identical to the one in Colosseum's
  program, so its functions are found by matching the compiled colosseum-mb
  code with literal pools and branch offsets masked.
- **Call sites.** The script finds every call to `IdentifyFlash`,
  `ReadFlash`, `VerifyFlashSector` and `VerifyFlashSectorNBytes`, and reads
  the library's variables from `IdentifyFlash`'s literal pool.
- **The save step.** The link code calls `SavePhase(0)`, waits for the
  GameCube, then calls `SavePhase(1)`, with writes to the link register
  around them. The script finds that step, and checks that every code path
  able to write the save goes through it. The trade program passes that
  check. The Battle Mode program never writes the save; its write code is
  unreachable.
- **Memory.** It finds where the program's memory ends: the end of `.bss`,
  plus the save blocks the program allocates after it at random offsets. The
  shim must sit above all of that (0x0203E800 leaves at least 6 KB to spare).

`make_xd_tables.py` turns this into `xd_tables.c`, which holds only
addresses, sizes and hashes.

### The hook (`src/xdlink`)

1. **Emerald's game-code check** (8 bytes and one literal at 0x0816CE52) is
   replaced by a call to `LinkHook` through Emerald's own `_call_via_r0`.
2. **`LinkHook`** does Emerald's Colosseum swap exactly as before. For any
   other program, it checks whether the compressed data and the loader hash
   to a known XD program. If they do, it changes the loader's final jump to
   point at `XdPostLoad` and leaves the program's index in the loader's
   unused last word.
3. **XD's own loader** decompresses the program and jumps to `XdPostLoad`.
4. **`XdPostLoad`** hashes the decompressed program (everything after the
   header) and, if it matches:
   - copies the shim (the backend plus five small wrappers) to the top of
     EWRAM and fills in that program's addresses;
   - rewrites the program's `BL` instructions to call the wrappers instead
     of the flash library and the save step;
   - starts the program.

Anything that doesn't match exactly runs untouched, as it did before.

The first version recognised XD's programs by the header's game code. It
passed every emulator test, because the tests sent the file as it is on the
disc, and failed on hardware, because XD changes that field. The current
hook ignores the header, and the tests now send programs the way XD does.

## 6. The cart flasher (`src/flasher/main.c`)

The flasher is a multiboot program built for one cart. `make_flasher.py`
appends a data block made from the user's own dump:

- **Identity ranges.** Everything that must not change, with one hash over
  all of it.
- **Spans of new bytes.** The Colosseum program, the hook, and the changed
  link code.
- **Hash tables for every 4 KB chunk** around each span: one for the written
  state, and one for each state the cart may be in before. That is the
  dump, and the dump after each earlier version of the flasher.

On the GBA:

1. Check the identity hash and probe the chip. Read the erase block size
   from CFI, accepting 4 to 256 KB.
2. **Erase-size test.** It programs markers at the edges of a test block in
   a 1 MB empty area, erases the block, and checks exactly which markers went.
   A chip whose real blocks differ from what it reports is caught here,
   before any game data is touched.
3. For each block of each span:
   - skip it if it's already written;
   - otherwise copy it, with the new bytes in place, to a spare empty block
     and verify the copy by hash;
   - erase the game block, program it from the spare copy (through SRAM, as
     the cart's own code does), and verify it.
4. Erase the spare and test areas again.

Every block is always in a known state: an earlier state, written, or
recoverable from a verified spare copy. Any power cut is therefore finished
by running the flasher again. A block in no known state, without a matching
spare copy, stops the flasher before it changes anything.

## 7. The batteryless dumper (`src/flasher/bldumper.c`)

FIX94's GBA Link Cable Dumper is a Wii app that sends a small program to the
GBA and talks to it over the JOY bus.
`tools/make_batteryless_dumper.py` replaces that GBA program, inside the
user's own `boot.dol`, with one that speaks the same protocol (v1.6):

- **Backup** returns only the 128 KB save area as the "save", in about 12
  seconds.
- **Restore** validates the file (signatures and Emerald checksums), shows
  both saves, and writes only after L+R+A.

## 8. Testing (`src/test`)

Everything is tested by running the real compiled GBA code in an ARM
emulator (Unicorn) against simulated carts:

- **`sim_cart.c`** models the board types:
  - command sets, D0/D1 swap, dual chips;
  - CFI tables, including ones that report the wrong block size;
  - erase block sizes from 64 to 512 KB, locked blocks, and programming
    without an erase (flagged as a violation);
  - injected erase and program failures;
  - power cuts that leave the block being erased half erased and wipe SRAM.
- **The flasher** is tested on every board type and block size, with power
  cuts at over 50 points (two in a row as well), failures, wrong CFI tables
  and refusals. It is also tested starting from every earlier flashed state,
  checking which blocks it erases.
- **The Colosseum program** runs its real save code through both link save
  phases, with the GameCube's handshake played back.
- **XD, end to end:**
  - Emerald's own patched copyright-screen code receives XD's real programs
    (with the header changed as XD does) and runs the hook;
  - XD's loader decompresses each program and `XdPostLoad` installs the
    shim;
  - XD's own start-up and save code then read the save from simulated cart
    ROM and save a trade. That save is read back after a power cycle.
- **The dumper**'s Wii side is replayed from its source code, step by step.
