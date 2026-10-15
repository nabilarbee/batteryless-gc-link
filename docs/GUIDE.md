# User guide

Everything here is for a US Pokémon Emerald (BPEE) batteryless repro cart.
Read the [risks](../README.md#risks) first. All commands run from the
repository folder, with Python 3.

- [1. Dump the cart](#1-dump-the-cart)
- [2. Back up the save in seconds](#2-back-up-and-restore-the-save-in-seconds)
- [3. Flash the link fix](#3-flash-the-link-fix)
- [4. Check the result](#4-check-the-result)
- [5. Edit a save without the dumper](#5-edit-a-save-without-the-dumper-save-writer)
- [6. If something stops](#6-if-something-stops)
- [7. Disc tool](#7-disc-tool)

## 1. Dump the cart

Make a full ROM dump with
[GBA Link Cable Dumper](https://github.com/FIX94/gba-link-cable-dumper)
(about 50 minutes for a 32 MB cart). This is your backup of the whole cart
and the input for building the flasher.

- Dump **before** you flash anything, and keep that dump. The flasher must
  be built from a dump of the cart as it came (or of an identical cart).
- The save inside the dump doesn't matter to the flasher, so an old dump of
  the same cart works.

## 2. Back up and restore the save in seconds

`make_batteryless_dumper.py` turns FIX94's GBA Link Cable Dumper into a
batteryless edition: the same Wii app, with a new program for the GBA side.
With the cart inserted:

- **Y (backup save)** copies the save from the cart to the SD card in about
  12 seconds;
- **X (restore save)** writes the file back to the cart after you confirm on
  the GBA;
- A (dump game) and the BIOS dump work as before. Z (clear save) does nothing
  on these carts.

### Set it up once

1. Copy `boot.dol` from the dumper's folder on your SD card (in `apps`) to
   your PC.
2. `python tools/make_batteryless_dumper.py boot.dol gba-link-cable-dumper-bl`
3. Copy the new `gba-link-cable-dumper-bl` folder into `apps` on the SD card.
   It shows in the Homebrew Channel as "GBA Link Cable Dumper (batteryless)",
   next to the original, which stays as it was.

### Back up, edit, restore

1. **Back up.** Start the batteryless edition and connect the GBA as you do
   with the dumper. Press A on the controller, then **Y**. The save goes to
   `dumps/POKEMON EMER [BPEE01].sav` on the SD card. The dumper refuses if a
   file with that name is already there, so move old backups out first.
2. **Edit** (optional). Copy the `.sav` to your PC and keep an untouched
   copy. Edit it in PKHeX, export it under the same name and put it back in
   `dumps` on the SD card.
3. **Restore.** Start the batteryless edition again, press A, then **X**.
4. **Look at the GBA.** It shows the save on the cart and the one from the SD
   card (name, ID, play time). Hold L+R and press A to write, or B to cancel.
   Writing takes about 10 seconds. The TV says "Save restored!" either way:
   the GBA screen says what really happened.
5. Turn the GBA off and start Emerald to check the save.

A restore erases and rewrites only the 128 KB save area. It finds that area
through the cart's own batteryless code, so it works even after an
interrupted write. If a restore stops partway, restore again before playing.

## 3. Flash the link fix

1. **Charge the GBA.** Back up the save (section 2).
2. **Build the flasher** from your dump:

   ```
   python tools/make_flasher.py your_dump.gba tools/colosseum-mb-repro.bps emerald_flasher.gba
   ```

   It refuses dumps that aren't US Emerald, or whose Colosseum program was
   already changed (a dump made after flashing).
3. **Copy** `emerald_flasher.gba` to the `gba` folder of the SD card, where
   GBA Link Cable ROM Sender looks for files.
4. **Send it.** GameCube controller in port 1, link cable in port 2. Start
   GBA Link Cable ROM Sender, select `emerald_flasher.gba`, press A. Start the
   GBA with the cart inserted while holding START+SELECT, or without the cart
   and insert it when the flasher asks.
5. **Read the first screen.** It should show:
   - `Cart matches your dump ok`
   - the chip type and block size, for example `Chip: AMD x16, D0/D1 swap`
     and `Blocks: 128 KB`
   - `Link code` and `Link programs`, each `to write` or `done`
   - `Will rewrite N blocks`: 3 on a cart never flashed, fewer if an earlier
     version already wrote some of them
   - `Save at FC0000: not touched`
6. **Hold L+R and press A.** Each block is copied to the spare area, then
   written, each with `ok`. It takes about a minute. **Don't turn the GBA
   off until it says `Done.`**

The flasher writes only these areas (with 128 KB blocks):

| Area | Contents |
|---|---|
| 0x160000–0x17FFFF | Emerald's link code: 8 bytes and one address changed to call the hook |
| 0x9A0000–0x9DFFFF | Emerald's Colosseum program (replaced by the repro build), and in its unused end the hook and the XD save code |
| 0xE40000… and 0xE80000–0xF7FFFF | Empty areas, used for a spare copy and an erase test, then erased again |

The save area and the cart's batteryless code are never written.

## 4. Check the result

From the safest check to the last:

1. Start Emerald and check that the save loads.
2. Colosseum: open the GBA transfer menu. Your team should appear.
3. XD: open the GBA trade screen, or Battle Mode with the GBA. Your team
   should appear. This only reads the save.
4. Do one small trade, then check the result in Emerald.

## 5. Edit a save without the dumper (save writer)

The batteryless dumper replaces this for most uses. The save writer works
from a full ROM dump instead.

1. Dump the cart (section 1). Dump again if you have played since.
2. `python tools/extract_save.py your_dump.gba emerald.sav`. It prints the
   trainer, ID and play time.
3. Edit `emerald.sav` in PKHeX and export it, for example as
   `emerald_edited.sav`.
4. `python tools/make_save_writer.py your_dump.gba emerald_edited.sav emerald_save_writer.gba`
   (any dump of this cart works, made before or after flashing).
5. Send `emerald_save_writer.gba` like the flasher.
6. The screen shows the save on the cart and the new one. Hold L+R and press
   A. It takes about 10 seconds. **Don't turn the GBA off until it says
   `Done.`** If it stops partway, send it again before playing Emerald: it
   finishes from its own copy of the save.

## 6. If something stops

| Screen says | What to do |
|---|---|
| "Nothing was written" | It refused, for the reason shown (a different cart, an unknown chip, an unsupported block layout). Your cart is unchanged. |
| An error with "Press A to try again" | Press A. If the error keeps coming back, or the power went off, send the program again: it continues where it stopped. |
| "Game data is intact" | It stopped before changing anything that matters. |
| "half written" on the flasher's first screen | A run was cut off. Hold L+R and press A: it finishes from its spare copy. |
| Emerald doesn't start after a cut-off flash | Send the flasher again (it doesn't need Emerald to start) and let it finish. |
| Colosseum or XD shows a save error | The cart is unchanged by it: the link programs only ever write the save area, and a failed trade leaves the previous save in place. Open an issue with a photo. |

## 7. Disc tool

`tools/patch_colosseum_iso.py` works on GameCube disc images:

- `scan ISO` lists the GBA programs inside a disc's `.fsys` archives;
- `extract ISO FOLDER` copies every GBA program on a disc into a folder,
  for analysis;
- `patch ISO PROGRAM OUTPUT_ISO` replaces Colosseum's GBA program on a copy
  of the disc (an older approach, not needed with the flasher).

The original ISO is only read.
