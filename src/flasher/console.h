#ifndef GUARD_CONSOLE_H
#define GUARD_CONSOLE_H

// 30x20 text console on BG0.

enum
{
    COLOR_WHITE,
    COLOR_GREEN,
    COLOR_RED,
    COLOR_YELLOW,
    COLOR_GRAY,
};

#define CON_COLS 30
#define CON_ROWS 20

void Con_Init(void);
void Con_SetColor(u32 color);
void Con_Putc(u32 c);
void Con_Print(const char *s);
void Con_Hex(u32 value, u32 digits);
void Con_Dec(u32 value);
void Con_ClearLine(void);   // blank the cursor's row and return to its start
void Con_Bar(u32 done, u32 total); // progress bar on the cursor's row, from the cursor

#endif // GUARD_CONSOLE_H
