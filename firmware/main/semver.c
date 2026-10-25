#include "semver.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* Drops trailing `git describe` decoration from a pre-release string in place:
 * "-dirty", then "-<commits>-g<sha>" (the whole string when nothing else is
 * left, e.g. "3-gabc1234" after "v1.2.3-3-gabc1234"). */
static void strip_describe_suffix(char *pre)
{
    size_t len = strlen(pre);
    if (strcmp(pre, "dirty") == 0) {
        pre[0] = '\0';  /* "v1.2.3-dirty": nothing before the marker */
        return;
    }
    if (len >= 6 && strcmp(pre + len - 6, "-dirty") == 0) {
        len -= 6;
        pre[len] = '\0';
    }
    size_t i = len;
    while (i > 0 && isxdigit((unsigned char)pre[i - 1])) {
        i--;
    }
    if (i == len || i < 1 || pre[i - 1] != 'g') {
        return;
    }
    size_t g = i - 1;            /* index of 'g' */
    if (g < 2 || pre[g - 1] != '-') {
        return;
    }
    size_t d = g - 1;            /* index of '-' before 'g' */
    size_t c = d;
    while (c > 0 && isdigit((unsigned char)pre[c - 1])) {
        c--;
    }
    if (c == d) {
        return;                  /* no commit count */
    }
    if (c == 0) {
        pre[0] = '\0';          /* only decoration: "3-gabc1234" */
    } else if (pre[c - 1] == '-') {
        pre[c - 1] = '\0';      /* "rc.1-3-gabc1234" -> "rc.1" */
    }
}

static bool parse(const char *s, semver_t *out)
{
    if (s == NULL) {
        return false;
    }
    if (*s == 'v' || *s == 'V') {
        s++;
    }
    int consumed = 0;
    memset(out, 0, sizeof(*out));
    if (sscanf(s, "%d.%d.%d%n", &out->major, &out->minor, &out->patch, &consumed) < 3) {
        return false;
    }
    s += consumed;
    if (*s == '-') {
        s++;
        if (strlen(s) >= sizeof(out->pre)) {
            return false;
        }
        strcpy(out->pre, s);
    } else if (*s != '\0') {
        return false;
    }
    return true;
}

bool semver_parse(const char *s, semver_t *out)
{
    return parse(s, out);
}

bool semver_parse_build(const char *s, semver_t *out)
{
    if (!parse(s, out)) {
        return false;
    }
    strip_describe_suffix(out->pre);
    return true;
}

static bool all_digits(const char *s, size_t len)
{
    if (len == 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)s[i])) {
            return false;
        }
    }
    return true;
}

/* Compares two pre-release strings per semver 2.0.0 section 11. */
static int pre_cmp(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        const char *ae = strchr(a, '.');
        const char *be = strchr(b, '.');
        size_t al = ae ? (size_t)(ae - a) : strlen(a);
        size_t bl = be ? (size_t)(be - b) : strlen(b);
        bool an = all_digits(a, al);
        bool bn = all_digits(b, bl);
        int c;
        if (an && bn) {
            /* Compare by magnitude without converting, so arbitrarily long
             * identifiers cannot saturate an integer. */
            const char *ad = a;
            const char *bd = b;
            size_t adl = al;
            size_t bdl = bl;
            while (adl > 1 && *ad == '0') { ad++; adl--; }
            while (bdl > 1 && *bd == '0') { bd++; bdl--; }
            if (adl != bdl) {
                c = adl < bdl ? -1 : 1;
            } else {
                c = strncmp(ad, bd, adl);
                c = c < 0 ? -1 : (c > 0 ? 1 : 0);
            }
        } else if (an != bn) {
            c = an ? -1 : 1; /* numeric identifiers sort below alphanumeric */
        } else {
            size_t n = al < bl ? al : bl;
            c = strncmp(a, b, n);
            if (c == 0) {
                c = al < bl ? -1 : (al > bl ? 1 : 0);
            }
        }
        if (c != 0) {
            return c;
        }
        a = ae ? ae + 1 : a + al;
        b = be ? be + 1 : b + bl;
    }
    if (*a == '\0' && *b == '\0') {
        return 0;
    }
    return *a == '\0' ? -1 : 1; /* the shorter set of identifiers sorts lower */
}

int semver_cmp(const semver_t *a, const semver_t *b)
{
    if (a->major != b->major) {
        return a->major - b->major;
    }
    if (a->minor != b->minor) {
        return a->minor - b->minor;
    }
    if (a->patch != b->patch) {
        return a->patch - b->patch;
    }
    bool ar = a->pre[0] == '\0';
    bool br = b->pre[0] == '\0';
    if (ar || br) {
        return ar == br ? 0 : (ar ? 1 : -1);
    }
    return pre_cmp(a->pre, b->pre);
}

void semver_best_consider(semver_best_t *best, const char *tag)
{
    semver_t ver;
    if (tag == NULL || strlen(tag) >= sizeof(best->tag) || !semver_parse(tag, &ver)) {
        return;
    }
    if (!best->have || semver_cmp(&ver, &best->ver) > 0) {
        strcpy(best->tag, tag);
        best->ver = ver;
        best->have = true;
    }
}
