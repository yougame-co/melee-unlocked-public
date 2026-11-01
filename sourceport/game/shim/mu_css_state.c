/* Read-only CSS diagnostics. The caller resolves the live icon table and English name table. */
#ifdef MU_NATIVE
#include "mu_shim.h"
#include <string.h>

/* Host CRT declarations: the decomp's MSL headers do not declare these. */
int snprintf(char* dst, __SIZE_TYPE__ size, const char* format, ...);
void* malloc(__SIZE_TYPE__ size);
void free(void* ptr);

static int previous_doors[4][4];
static int previous_valid;

void mu_css_state_reset(void)
{
    previous_valid = 0;
}

/* The retail English table is full-width CP932. Fold its Latin letters and punctuation to
 * ASCII without inventing another fighter-name table. MEX/HP names may already be ASCII.
 * Escape all other bytes, quotes and controls so a disc name cannot break the JSON line. */
static char* json_name(char* out, const char* name)
{
    const unsigned char* s = (const unsigned char*) name;
    *out++ = '"';
    while (*s) {
        unsigned c = *s++;
        if (c == 0x82 && *s >= 0x4F && *s <= 0x58) c = '0' + *s++ - 0x4F;
        else if (c == 0x82 && *s >= 0x60 && *s <= 0x79) c = 'A' + *s++ - 0x60;
        else if (c == 0x82 && *s >= 0x81 && *s <= 0x9A) c = 'a' + *s++ - 0x81;
        else if (c == 0x81) {
            unsigned ascii = 0;
            switch (*s) {
            case 0x40: ascii = ' '; break;
            case 0x43: ascii = ','; break;
            case 0x44: ascii = '.'; break;
            case 0x7C: ascii = '-'; break;
            case 0x95: ascii = '&'; break;
            }
            if (ascii) { c = ascii; ++s; }
        }
        if (c == '"' || c == '\\') { *out++ = '\\'; *out++ = (char) c; }
        else if (c < 0x20 || c >= 0x7F) { snprintf(out, 7, "\\u%04x", c); out += 6; }
        else *out++ = (char) c;
    }
    *out++ = '"';
    *out = 0;
    return out;
}

/* doors[p] = { p_kind, sel_icon, resolved character kind or -1, coin placed }. Comparing the
 * icon as well as fighter catches two icons resolving to the same fighter, and live icon swaps. */
void mu_css_state_log(int major, int minor, const int doors[4][4], const char* const names[4])
{
    size_t capacity = 512;
    char *line, *out;
    int i;
    if (previous_valid && memcmp(previous_doors, doors, sizeof previous_doors) == 0) return;
    if (mu_host == NULL || mu_host->log == NULL) return;
    for (i = 0; i < 4; ++i) {
        if (doors[i][0] != 3 && doors[i][2] >= 0 && names[i] != NULL)
            capacity += strlen(names[i]) * 6; /* worst-case JSON escaping, no name truncation */
    }
    line = (char*) malloc(capacity);
    if (line == NULL) return;
    out = line + snprintf(line, capacity, "css-state: {\"major\":%d,\"minor\":%d", major, minor);
    for (i = 0; i < 4; ++i) {
        int kind = doors[i][0];
        out += snprintf(out, capacity - (out - line), ",\"p%d\":{\"kind\":\"", i + 1);
        if (kind == 0 || kind == 1 || kind == 3)
            out += snprintf(out, capacity - (out - line), "%s", kind == 0 ? "HMN" : kind == 1 ? "CPU" : "closed");
        else out += snprintf(out, capacity - (out - line), "unknown:%d", kind);
        *out++ = '"';
        if (kind != 3) {
            if (doors[i][2] >= 0) {
                out += snprintf(out, capacity - (out - line), ",\"fighter\":%d", doors[i][2]);
                if (names[i] != NULL) {
                    memcpy(out, ",\"name\":", 8); out += 8;
                    out = json_name(out, names[i]);
                }
            }
            out += snprintf(out, capacity - (out - line), ",\"locked\":%s", doors[i][3] ? "true" : "false");
        }
        *out++ = '}';
    }
    *out++ = '}';
    *out = 0;
    /* Same host log path as OSReport (mu_os.c), without its 512-byte truncation. */
    mu_host->log(line);
    free(line);
    memcpy(previous_doors, doors, sizeof previous_doors);
    previous_valid = 1;
}
#endif
