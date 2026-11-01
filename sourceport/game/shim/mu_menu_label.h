/* Shared baked menu art and native SIS description parsing. */
#ifndef MU_MENU_LABEL_H
#define MU_MENU_LABEL_H
#include "mu_menu_label_art.h"

/* SIS command lengths from HSD_SisLib_803A6478. Read operands, including
 * embedded zeroes, rather than treating an encoded stream as a C string. */
static int mu_menu_sis_command_size(unsigned op)
{
    switch (op) {
    case 5: return 3;
    case 6: case 7: case 8: case 9: case 10: case 14: return 5;
    case 12: return 4;
    default: return 1;
    }
}

/* Copy the actual row's formatting through its first glyph. This preserves
 * alignment, colour, 0x0e scale, 0x07 origin/baseline, delay and fitting.
 * Reject jumps: their relocated operands must never be copied into owned text. */
static int mu_menu_description_prefix(const unsigned char* stream)
{
    int n = 0;
    if (!stream) return 0;
    while (n < 128) {
        unsigned op = stream[n];
        if (op >= 0x20 || op == 0x1a) return n;
        if (!op || op == 8 || op == 9) return 0;
        n += mu_menu_sis_command_size(op);
    }
    return 0;
}
#endif
