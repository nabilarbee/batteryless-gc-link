# batteryless-gc-link

Make **Pokémon Colosseum** and **Pokémon XD** work with **batteryless repro
(bootleg) Pokémon Emerald cartridges** over the GBA–GameCube link cable. No
cart writer, no modified discs, and no ROMs or ISOs in this repository.

Batteryless repro carts keep the save inside their ROM flash chip instead of
a battery-backed save chip. Emerald itself is patched to cope with that, but
the GameCube games send their own GBA programs, which look for a save chip
that isn't there. Trades fail and your team never shows up. This project adds
the missing support and installs it into the cart over the same link cable,
from a Wii or GameCube.

|Tool|What it does|
|-|-|
|**Cart flasher**|A GBA program, sent over the link cable, that writes the fix into your cart's ROM: a repro build of Emerald's Colosseum link program, and a hook that fixes XD's programs as they arrive|
|**Batteryless dumper**|A batteryless edition of FIX94's GBA Link Cable Dumper: backs up and restores the cart's save in about 12 seconds, instead of a 50-minute ROM dump|
|**Save writer**|Writes an edited save (for example from PKHeX) back to the cart, without the dumper|
|**Disc tool**|Lists and extracts the GBA programs on a GameCube disc image|

## Status

Tested on one cart: a US Pokémon Emerald (BPEE) batteryless repro with an
AMD-style x16 flash chip (data lines D0/D1 swapped), 128 KB erase blocks and
the save at ROM offset 0xFC0000. Setup: Japanese Wii (RVL-001) with
Nintendont, official DOL-011 link cable, GBA SP, US Colosseum (GC6E01) and US
XD (GXXE01).

|Feature|Real hardware|Emulator tests|
|-|-|-|
|Colosseum: trading, team shown|✅ works|✅|
|XD: trading screen, Battle Mode with the GBA|✅ works|✅|
|Cart flasher, including updates of earlier flashes|✅ works|✅ power cuts at 50+ points|
|Save backup with the batteryless dumper|✅ works (Emerald and FireRed carts)|✅|
|Save restore (dumper or save writer)|not tried yet|✅|
|Other board types (Intel buffered/word, dual AMD x8)|not tried|✅|
|FireRed/LeafGreen link, Pokémon Box|not supported yet|—|

The version history, including what failed on hardware and why, is in
[CHANGELOG.md](CHANGELOG.md).

## Who this is for

* Owners of batteryless repro Pokémon Emerald carts (the kind sold on
AliExpress and similar) who want to use them with Colosseum or XD on a
GameCube or a Wii with Nintendont.
* Anyone who wants to back up, edit and restore the save on such a cart
without buying a cart reader.
* Developers curious about GBA multiboot, GameCube link programs, NOR flash
programming and the bootleg batteryless patch. See
[docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md).

## What you need

* The repro cart, and a full ROM dump of it made with
[GBA Link Cable Dumper](https://github.com/FIX94/gba-link-cable-dumper)
(the dump is your backup and the input for building the flasher).
* A Wii with the Homebrew Channel (or a GameCube that runs homebrew), an
official GameCube–GBA link cable, and a GBA or GBA SP.
* [GBA Link Cable ROM Sender](https://wiibrew.org/wiki/GBA_Link_Cable_ROM_Sender)
to send the flasher and save writer.
* Your own copies of the games (Colosseum and XD discs or your own backups).
* A PC with Python 3. The scripts need no extra packages.

## Quick start

Step by step instructions, with what each screen should say, are in
[docs/GUIDE.md](docs/GUIDE.md). In short:

1. Dump the cart with GBA Link Cable Dumper and keep the dump safe.
2. Back up the save with the batteryless dumper (`tools/make\_batteryless\_dumper.py`).
3. Build the flasher from your dump:
`python tools/make\_flasher.py your\_dump.gba tools/colosseum-mb-repro.bps emerald\_flasher.gba`
4. Send `emerald\_flasher.gba` with GBA Link Cable ROM Sender, check the first
screen, hold L+R and press A, wait for `Done.`
5. Check Emerald, then Colosseum, then XD.

## How it works

* **The save backend.** It finds the Gen 3 save image inside the cart's ROM
(checked by sector signatures and checksums), reads it straight from ROM,
and writes changes with the flash chip's own commands. It uses the cart's
128 KB banked SRAM as a working copy, the same way the cart's own
batteryless code does. Writes follow the games' two-phase link save, so an
interrupted trade leaves the previous save valid.
* **Colosseum.** Emerald swaps in its own copy of the Colosseum link program
when Colosseum connects. The flasher replaces that copy with a build of
[pret/colosseum-mb](https://github.com/pret/colosseum-mb) (a decompilation
of that program) with the save backend linked in.
* **XD.** XD sends its own compressed programs, and Emerald runs them as
sent. The flasher adds a small hook to Emerald's link code. The hook
recognises XD's programs by hash and lets their own loader decompress them.
It then copies the save backend into free memory and redirects the
programs' flash calls to it. The XD disc is never modified, and no XD code
is stored on the cart or in this repository.
* **The flasher.** A multiboot program that rewrites only the blocks it has
to, with every safety check described below.

The full technical write-up is in [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md).

## Safety design

* **Nothing is written to a cart that doesn't match the dump** the flasher
was built from. A hash over everything that must not change is checked
first.
* **The flash chip is identified, and its erase block size read from CFI.**
The size is then tested by experiment in an empty area before any game
data is touched. Unknown chips are refused.
* **Each block is first copied, with the new bytes, to a spare empty block**
and checked there. Only then is the game block erased and rewritten from
that copy, and checked again.
* **If the power goes off, sending the flasher again carries on** from
whatever state the cart is in. It recognises every state an earlier
version of the flasher could have left.
* **The save area and the cart's own batteryless code are never written by
the flasher.**
* **You confirm before anything is written** (hold L+R, press A).

## Risks

**Use this at your own risk.** It writes to your cart's flash memory, and
neither the authors nor the tools can guarantee your hardware behaves like
the one it was tested on.

* **Game data.** A bug, an unexpected chip or a power cut could leave
Emerald's ROM damaged. The flasher is built to recover from power cuts:
send it again and it finishes the job, and emulator tests cut the power at
over 50 points. But if something else goes wrong, getting the cart back may
need a cart writer (such as a GBxCart RW) and your full ROM dump. **Make
and keep that dump before you flash.**
* **While a block is being rewritten, the cart is incomplete.** If one of
the blocks holds Emerald's link code (the first flash), Emerald won't start
until you send the flasher again. Updating a cart that already has the hook
only rewrites a block Emerald doesn't need to start.
* **Your save.** Every save on these carts (in Emerald itself, and through
Colosseum or XD) erases and rewrites the 128 KB save area, which takes 1 to
3 seconds. A power cut in that window can lose the save. Back it up with
the batteryless dumper before trading.
* **Trades take a little longer.** Each phase of a link save waits for the
ROM flash to be erased and programmed. The GameCube tolerated this in
every test. If it ever times out, the trade fails and the previous save
stays as it was.
* **Only one kind of cart has been tested on hardware** (see Status).
Others are covered by emulator tests, or refused if the board type isn't
known.

## Limitations

* US Pokémon Emerald (BPEE) only. FireRed/LeafGreen and other languages
aren't supported by the flasher yet. The batteryless dumper's backup has
worked on an Emerald and a FireRed batteryless cart.
* Pokémon Box isn't handled yet.
* **Batteryless repro carts have no working clock**, so berries never grow
and other clock-based events (tides, daily events) are frozen. That comes
from the cart and is unrelated to this project. On the tested cart, the
repro maker had disabled Emerald's "internal battery has run dry" message.

## Legal

* No ROMs, disc images, BIOS files or game code are included. You need your
own cart and discs. The Colosseum fix ships as a BPS patch applied to the
copy of the program in your own dump. The XD fix contains only addresses,
sizes and hashes of XD's programs.
* This is a fan project, not affiliated with or endorsed by Nintendo, Game
Freak, Creatures, The Pokémon Company or Genius Sonority. Pokémon and all
related names are trademarks of their respective owners.

## Repository layout

|Path|Contents|
|-|-|
|`tools/`|What users run: the Python scripts and the prebuilt GBA programs (`\*\_code.bin`, `linkhook.bin`) and patch (`colosseum-mb-repro.bps`) they use|
|`src/flasher/`|GBA side of the flasher, save writer and batteryless dumper (C, ARM assembly)|
|`src/xdlink/`|The XD hook and save shim, and the analyser that finds what to redirect in XD's programs|
|`src/repro/`|The save backend shared by everything: save scan, chip drivers, SRAM working copy, commit|
|`src/test/`|Simulated flash carts (C) and the emulator tests (Python and Unicorn)|
|`patches/colosseum-mb/`|The full history as `git am` patches against pret/colosseum-mb `65ef452`: the sources above in place, plus the changes to the Colosseum program|
|`docs/`|User guide, technical write-up, building and testing|

## Building and testing

The prebuilt files in `tools/` are all you need to use the project. To
rebuild them or run the tests, see
[docs/BUILDING-AND-TESTING.md](docs/BUILDING-AND-TESTING.md).

## Credits

* [pret/colosseum-mb](https://github.com/pret/colosseum-mb),
[pokeemerald](https://github.com/pret/pokeemerald) and
[pokefirered](https://github.com/pret/pokefirered): the decompilations
this builds on.
* [FIX94](https://github.com/FIX94): GBA Link Cable Dumper and ROM Sender
(MIT). The batteryless dumper replaces only the dumper's GBA side.
* [DanOl98/pkmn\_gba\_batteryless\_patcher](https://github.com/DanOl98/pkmn_gba_batteryless_patcher):
documentation of the bootleg batteryless patch and its board types.
* [StarsMmd's GoD Tool](https://github.com/PekanMmd/Pokemon-XD-Code): the
Colosseum/XD disc archive formats.
* [GBATEK](https://problemkaputt.de/gbatek.htm),
[font8x8](https://github.com/dhepper/font8x8) (public domain) and the
[Unicorn engine](https://www.unicorn-engine.org/) for the tests.
* Helped with Claude finding the right tools/resources to use when I gave the goals and constraints.
* Tested on my own owned real hardware.

## License

MIT, see [LICENSE](LICENSE). This covers the code in this repository. The
patches in `patches/colosseum-mb/` also modify files of pret/colosseum-mb,
which carries no license file of its own; only the changes are covered here.

