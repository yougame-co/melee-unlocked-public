/* Run the production formatter with a capturing host log, without a disc or game process. */
#include "mu_host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mu_css_state_reset(void);
void mu_css_state_log(int major, int minor, const int doors[4][4], const char* const names[4]);

static char last[16384];
static int logs, checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); abort(); } } while (0)
static void capture(const char* line)
{
    CHECK(strlen(line) < sizeof last);
    strcpy(last, line);
    ++logs;
}
static const MuHostApi host = { .log = capture };
const MuHostApi* mu_host = &host;

int main(void)
{
    int doors[4][4] = { {0, 26, 34, 1}, {1, 2, 2, 0}, {3, 255, -1, 0}, {2, 255, -1, 0} };
    const char* names[4] = { "Wolf", "\x82\x65\x82\x8f\x82\x98", NULL, NULL }; /* full-width Fox */
    mu_css_state_reset();
    mu_css_state_log(2, 0, doors, names);
    CHECK(logs == 1);
    CHECK(strcmp(last, "css-state: {\"major\":2,\"minor\":0,\"p1\":{\"kind\":\"HMN\",\"fighter\":34,\"name\":\"Wolf\",\"locked\":true},"
                       "\"p2\":{\"kind\":\"CPU\",\"fighter\":2,\"name\":\"Fox\",\"locked\":false},"
                       "\"p3\":{\"kind\":\"closed\"},\"p4\":{\"kind\":\"unknown:2\",\"locked\":false}}") == 0);
    for (int i = 0; i < 120; ++i) mu_css_state_log(2, 0, doors, names);
    CHECK(logs == 1);
    /* Each source field independently causes a log, including an icon-only change. */
    for (int field = 0; field < 4; ++field) {
        ++doors[0][field];
        mu_css_state_log(2, 0, doors, names);
        CHECK(logs == field + 2);
        mu_css_state_log(2, 0, doors, names);
        CHECK(logs == field + 2);
    }
    mu_css_state_reset();
    mu_css_state_log(5, 3, doors, names);
    CHECK(logs == 6); /* identical picks on another CSS entry still produce one line */
    CHECK(strstr(last, "\"major\":5,\"minor\":3") != NULL);
    doors[0][0] = 0;
    doors[0][2] = -1;
    names[0] = "ignored out-of-range icon";
    mu_css_state_log(5, 3, doors, names);
    CHECK(strstr(last, "\"p1\":{\"kind\":\"HMN\",\"locked\":true}") != NULL);
    doors[0][2] = 3;
    names[0] = "Mr. \"Game\" \\ Watch\n\t";
    mu_css_state_log(5, 3, doors, names);
    CHECK(strstr(last, "\"name\":\"Mr. \\\"Game\\\" \\\\ Watch\\u000a\\u0009\"") != NULL);
    CHECK(strchr(last, '\n') == NULL);
    char long_name[1001];
    memset(long_name, 'W', sizeof long_name - 1);
    long_name[sizeof long_name - 1] = 0;
    for (int i = 0; i < 4; ++i) {
        doors[i][0] = 1;
        doors[i][2] = 34;
        names[i] = long_name;
    }
    mu_css_state_log(2, 0, doors, names);
    CHECK(strlen(last) > 4000); /* bypass OSReport's 512-byte limit without truncating JSON */
    CHECK(strcmp(last + strlen(last) - 2, "}}") == 0);
    /* Closed doors expose no stale fighter/name/lock; missing names retain valid fighter ids. */
    doors[0][0] = 3;
    names[1] = NULL;
    mu_css_state_log(2, 0, doors, names);
    CHECK(strstr(last, "\"p1\":{\"kind\":\"closed\"}") != NULL);
    CHECK(strstr(last, "\"p2\":{\"kind\":\"CPU\",\"fighter\":34,\"locked\":false}") != NULL);
    mu_host = NULL;
    ++doors[0][1];
    int before = logs;
    mu_css_state_log(2, 0, doors, names);
    CHECK(logs == before);
    mu_host = &host;
    mu_css_state_log(2, 0, doors, names);
    CHECK(logs == before + 1);
    printf("css_state: %d checks passed\n", checks);
    return 0;
}
