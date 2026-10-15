// Minimal text console: mode 0, BG0, 4bpp font tiles 0-95 for ASCII 0x20-0x7F.
#include "repro_types.h"
#include "gba/io_reg.h"
#include "console.h"
#include "font_data.h"

#define CHAR_BASE  ((vu32 *)0x06000000)
#define SCREEN     ((vu16 *)0x0600F800)   // screen base block 31
#define BG_PALETTE ((vu16 *)0x05000000)
#define RGB(r, g, b) ((r) | ((g) << 5) | ((b) << 10))

static u8 sX;
static u8 sY;
static u16 sAttr;

static const u16 sColors[] =
{
    [COLOR_WHITE]  = RGB(31, 31, 31),
    [COLOR_GREEN]  = RGB(10, 31, 10),
    [COLOR_RED]    = RGB(31, 10, 8),
    [COLOR_YELLOW] = RGB(31, 29, 6),
    [COLOR_GRAY]   = RGB(19, 19, 21),
};

void Con_Init(void)
{
    u32 i;

    REG_DISPCNT = 0x0080; // forced blank while VRAM is set up
    for (i = 0; i < 96 * 8; i++)
        CHAR_BASE[i] = gFontTiles[i];
    for (i = 0; i < 32 * 32; i++)
        SCREEN[i] = 0;
    BG_PALETTE[0] = RGB(1, 3, 9);
    for (i = 0; i < sizeof(sColors) / sizeof(sColors[0]); i++)
        BG_PALETTE[i * 16 + 1] = sColors[i];
    REG_BG0CNT = 31 << 8;
    REG_BG0HOFS = 0;
    REG_BG0VOFS = 0;
    REG_DISPCNT = DISPCNT_MODE_0 | DISPCNT_BG0_ON;
    sX = 0;
    sY = 0;
    sAttr = 0;
}

void Con_SetColor(u32 color)
{
    sAttr = color << 12;
}

static void NewLine(void)
{
    u32 i;

    sX = 0;
    if (sY < CON_ROWS - 1)
    {
        sY++;
        return;
    }
    for (i = 0; i < 32 * (CON_ROWS - 1); i++)
        SCREEN[i] = SCREEN[i + 32];
    for (i = 0; i < 32; i++)
        SCREEN[32 * (CON_ROWS - 1) + i] = 0;
}

void Con_Putc(u32 c)
{
    if (c == '\n')
    {
        NewLine();
        return;
    }
    if (c < 0x20 || c > 0x7F)
        c = '?';
    SCREEN[sY * 32 + sX] = (c - 0x20) | sAttr;
    if (++sX == CON_COLS)
        NewLine();
}

void Con_Print(const char *s)
{
    while (*s)
        Con_Putc(*s++);
}

void Con_Hex(u32 value, u32 digits)
{
    static const char hex[] = "0123456789ABCDEF";

    while (digits--)
        Con_Putc(hex[(value >> (digits * 4)) & 0xF]);
}

void Con_Dec(u32 value)
{
    static const u32 powers[] = {1000000000, 100000000, 10000000, 1000000, 100000, 10000, 1000, 100, 10, 1};
    u32 i, digit;
    bool32 started = FALSE;

    for (i = 0; i < sizeof(powers) / sizeof(powers[0]); i++)
    {
        digit = 0;
        while (value >= powers[i])
        {
            value -= powers[i];
            digit++;
        }
        if (digit || started || powers[i] == 1)
        {
            Con_Putc('0' + digit);
            started = TRUE;
        }
    }
}

void Con_ClearLine(void)
{
    u32 i;

    for (i = 0; i < 32; i++)
        SCREEN[sY * 32 + i] = 0;
    sX = 0;
}

void Con_Bar(u32 done, u32 total)
{
    u32 width = CON_COLS - sX;
    u32 filled = 0;
    u32 i;

    // filled = done * width / total, without division
    if (total)
    {
        u32 acc = 0;
        for (i = 0; i < width; i++)
        {
            acc += total;
            if (acc <= done * width)
                filled++;
        }
    }
    for (i = 0; i < width; i++)
        SCREEN[sY * 32 + sX + i] = (i < filled ? 0x7F - 0x20 : '.' - 0x20) | (COLOR_GRAY << 12);
}
