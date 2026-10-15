# Changelog

Each version was a release tested on the author's cart (see the Status
section of the README). Dates are in UTC+8.

## v0.6 (2026-10-14)

- **XD hook fixed.** When XD sends a program, it writes its own value over the
  header's game code. v0.5's hook looked for the value stored on the disc, so
  on hardware it never recognised XD's programs. They ran unchanged and said
  "The save file has been deleted". The hook now recognises the programs by a
  hash of the compressed program and its loader, and ignores the header. The
  emulator tests now send programs with the header changed, as XD does.
- **Flasher v0.4.** It writes up to four spans and recognises every state an
  earlier version could have left: the dump as it came, after v0.2, and after
  v0.5. An update from v0.5 rewrites only the block that holds the hook.
- On hardware: XD's trading screen and Battle Mode with the GBA work,
  Colosseum still works, and the save backup works.

## v0.5 (2026-10-14)

- XD link fix: a hook in Emerald's link code, and a save shim installed into
  XD's programs as they arrive (`src/xdlink`). An analyser finds what to
  redirect in each program and checks that it is safe.
- Flasher v0.3 writes the hook as well, in one or more spans.
- Disc tool: new `extract` command.
- On hardware: Colosseum kept working. XD's programs downloaded but said "The
  save file has been deleted" (fixed in v0.6).

## v0.4 (2026-10-14)

- Batteryless edition of FIX94's GBA Link Cable Dumper: save backup and
  restore in about 12 seconds.
- On hardware: backup works on an Emerald and a FireRed batteryless cart.

## v0.3 (2026-10-14)

- Save writer: writes an edited save to the cart, built from a full ROM dump.

## v0.2 (2026-10-14)

- Cart flasher: writes the repro Colosseum program into the cart over the
  link cable. It checks the cart against the dump, tests the erase size,
  stages each block in a spare area, and recovers from power cuts.
- On hardware: the Colosseum link works.

## v0.1 (2026-10-13)

- Save backend for batteryless repro carts, linked into a build of
  pret/colosseum-mb (`REPRO=1`).
- Shipped as a BPS patch for the copy of the program in the user's own dump,
  with ROM and disc patch tools.
- Emulator tests on simulated carts.
