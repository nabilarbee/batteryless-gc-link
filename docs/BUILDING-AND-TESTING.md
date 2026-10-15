# Building and testing

You only need the prebuilt files in `tools/` to use the project. This page is
for rebuilding them, running the tests and adding support for more XD
programs.

## Where the sources live

The Colosseum fix is a build of [pret/colosseum-mb](https://github.com/pret/colosseum-mb),
so all the code is developed inside a clone of that repository.
`patches/colosseum-mb/` holds the full history as `git am` patches against
commit `65ef452`.

`src/` and `tools/` hold copies of the same files, so you can read them on
GitHub. Their Makefiles use paths inside the colosseum-mb tree, so build from
the patched clone.

| Here | In the patched colosseum-mb tree |
|---|---|
| `src/repro/*.c` | `payload/src/repro/` |
| `src/repro/*.h` | `payload/include/` |
| `src/flasher/` | `repro_tools/flasher/` |
| `src/xdlink/` | `repro_tools/xdlink/` |
| `src/test/` | `repro_tools/test/` |
| `tools/` | `repro_tools/`. `emerald_hook.py` is in `repro_tools/xdlink/` |

The patches also change the Colosseum program itself: `payload/src/agb_flash*.c`,
`payload/src/all4.c` and the Makefiles. When built with `REPRO=1`, it links in
the save backend.

## Requirements

- Linux or WSL, with `git`, `make`, a C compiler and Python 3.
- binutils for `arm-none-eabi`, for example `binutils-arm-none-eabi` on
  Debian and Ubuntu, or devkitARM.
- [agbcc](https://github.com/pret/agbcc), the compiler that pret's GBA
  decompilations use.
- For the emulator tests: `pip install unicorn` (tested with Unicorn 2.1.4).

## Build

```sh
git clone https://github.com/pret/colosseum-mb
cd colosseum-mb
git checkout -b batteryless 65ef452
git am /path/to/batteryless-gc-link/patches/colosseum-mb/*.patch

# agbcc goes into tools/agbcc, as for other pret projects
git clone https://github.com/pret/agbcc ../agbcc
(cd ../agbcc && ./build.sh && ./install.sh ../colosseum-mb)

make compare                 # the original program, checked against colosseum-mb-em.sha1
make repro                   # colosseum-mb-em-repro.gba and payload/payload-repro.elf
make -C repro_tools/xdlink   # linkhook.bin, copied to repro_tools/
make -C repro_tools/flasher  # flasher_code.bin, savewriter_code.bin, bldumper_code.bin
```

With pret's agbcc, the builds should match the shipped files:

| File | SHA-1 |
|---|---|
| `colosseum-mb-em-repro.gba` | `7539d32d183fe7b2604cc58dfccf63f140238f7f` |
| `flasher_code.bin` | `317425ccb686690a54493bbebd757dca78f14470` |
| `savewriter_code.bin` | `d2eac469711465ae4265da9bee506d48ef6ffbd1` |
| `bldumper_code.bin` | `62382854ffecba70e4b1ad4d003dea32c9715adf` |
| `linkhook.bin` | `b1407d4e439ec5a265375c0745397a92efee461b` |

### The Colosseum patch

`colosseum-mb-em.gba` is byte-identical to the copy of the program inside
Emerald (SHA-1 `6cf444e9…`). That is why a patch made between the two builds
applies to anyone's dump:

```sh
python3 repro_tools/bps.py create colosseum-mb-em.gba colosseum-mb-em-repro.gba colosseum-mb-repro.bps
```

A new patch file may differ byte for byte from the shipped one. What matters
is the program it produces:

```sh
python3 repro_tools/bps.py apply colosseum-mb-em.gba repro_tools/colosseum-mb-repro.bps check.gba
cmp check.gba colosseum-mb-em-repro.gba
```

### Other scripts in `tools/`

- `make_program.py DUMP BPS OUT.gba` extracts the program from a dump and
  applies the patch.
- `patch_rom.py DUMP BPS OUT.gba` writes the patched program into a copy of
  the ROM, for people with a cart writer.

The flasher made both of these unnecessary for the tested setup.

## Tests

```sh
make -C repro_tools/test check     # host tests of the save scan and commit; builds libsimcart.so
cd repro_tools/test
python3 emu_test.py                                # Colosseum repro program (needs make repro)
python3 flasher_emu_test.py DUMP.gba [SCENARIO ...] # flasher: board types, power cuts, updates
python3 savewriter_emu_test.py DUMP.gba
python3 bldumper_emu_test.py DUMP.gba
python3 patcher_test.py DUMP.gba [boot.dol]        # ROM, disc and dumper patch tools
python3 xd_emu_test.py DUMP.gba XD_PROGRAMS/       # XD end to end
```

- `DUMP.gba` is a full dump of a US Emerald batteryless repro cart. The
  tests build their simulated carts from it.
- `XD_PROGRAMS/` holds the GBA programs from your XD disc:
  `python3 repro_tools/patch_colosseum_iso.py extract XD.iso XD_PROGRAMS`.
- The flasher and XD tests run many scenarios and take a while. Name
  scenarios to run only those.

[HOW-IT-WORKS.md](HOW-IT-WORKS.md#8-testing-srctest) describes what the
tests cover.

## Adding XD programs

The shipped `xd_tables.c` covers the 12 GBA programs on the US disc
(GXXE01). Other discs are untested. If the analyser accepts their programs,
adding them takes these steps:

1. Extract the programs:
   `python3 repro_tools/patch_colosseum_iso.py extract XD.iso XD_PROGRAMS`.
2. Run `python3 repro_tools/xdlink/analyze_xd.py XD_PROGRAMS/*.gba`. For
   each program, it finds the flash library, every call to redirect and the
   save step. It also checks that every save write goes through that step,
   and that the shim's memory is free.
3. Regenerate the table:
   `python3 repro_tools/xdlink/make_xd_tables.py repro_tools/xdlink/xd_tables.c XD_PROGRAMS/*.gba`.
4. Rebuild the hook and run `xd_emu_test.py` with those programs.

## Rules for changing the hook

These rules come from bugs found during development:

- **Keep `HookEntry` at offset 0xD0 of `linkhook.bin`** (address
  0x089C20D1, set in `rom.ld`). Emerald's patched link code calls that
  address. With the address unchanged, an update rewrites only the block that
  holds the hook, and never the block that holds Emerald's link code, which
  Emerald needs to start.
- **Keep every released hook.** Copy the previous `linkhook.bin` into
  `tools/` and add it to `PREVIOUS_HOOKS` in `make_flasher.py`, so the
  flasher recognises carts flashed with it. Each span can know up to three
  earlier states (`MAX_SOURCES` in `config.h`).
- **Keep the shim between XD's memory and 0x0203FF80.** It must sit above
  all of the memory every XD program uses (`analyze_xd.py` checks this) and
  end below 0x0203FF80 (`shim.ld` checks this).
- **Test with programs as XD sends them**, not as they are stored on the
  disc. XD writes its own value over the header's game code, and the first
  hook failed on hardware because of it (see
  [HOW-IT-WORKS.md](HOW-IT-WORKS.md#5-xd)).
