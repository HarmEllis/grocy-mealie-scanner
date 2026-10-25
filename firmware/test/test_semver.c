/* Host unit tests for the OTA semver parsing and precedence. */

#include "semver.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            failures++;                                                     \
        }                                                                   \
    } while (0)

static int cmp(const char *a, const char *b)
{
    semver_t x, y;
    /* `a`/`b` go through the build parser, which is a superset of the strict
     * one for strings without describe decoration. */
    if (!semver_parse_build(a, &x) || !semver_parse_build(b, &y)) {
        printf("FAIL parse %s / %s\n", a, b);
        failures++;
        return 0;
    }
    int c = semver_cmp(&x, &y);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

static const char *pre_of(const char *s)
{
    static semver_t v;
    return semver_parse_build(s, &v) ? v.pre : "<parse error>";
}

static const char *strict_pre_of(const char *s)
{
    static semver_t v;
    return semver_parse(s, &v) ? v.pre : "<parse error>";
}

int main(void)
{
    semver_t v;
    CHECK(semver_parse("v1.2.3", &v) && v.major == 1 && v.minor == 2 && v.patch == 3 && v.pre[0] == '\0');
    CHECK(semver_parse("1.2.3", &v));
    CHECK(!semver_parse("1.2", &v));
    CHECK(!semver_parse("", &v));
    CHECK(!semver_parse(NULL, &v));
    CHECK(!semver_parse("v1.2.3+meta", &v));

    CHECK(strcmp(pre_of("v0.7.0-rc.1"), "rc.1") == 0);

    /* git describe decoration is build metadata, not a pre-release. */
    CHECK(strcmp(pre_of("v0.6.0-3-gabc1234"), "") == 0);
    CHECK(strcmp(pre_of("v0.6.0-3-gabc1234-dirty"), "") == 0);
    CHECK(strcmp(pre_of("v0.6.0-dirty"), "") == 0);
    CHECK(strcmp(pre_of("v0.7.0-rc.1-3-gabc1234"), "rc.1") == 0);
    CHECK(strcmp(pre_of("v0.7.0-rc.1-dirty"), "rc.1") == 0);
    /* Uppercase and mixed-case SHAs are decoration too (the workflow rejects
     * release tags ending that way). */
    CHECK(strcmp(pre_of("v0.7.0-rc.1-3-gABC"), "rc.1") == 0);
    CHECK(strcmp(pre_of("v0.7.0-rc.1-3-gAbC1d"), "rc.1") == 0);
    CHECK(strcmp(pre_of("v0.7.0-beta"), "beta") == 0);

    CHECK(cmp("v1.0.0", "v1.0.0") == 0);
    CHECK(cmp("v1.0.1", "v1.0.0") == 1);
    CHECK(cmp("v0.9.9", "v1.0.0") == -1);
    CHECK(cmp("v1.10.0", "v1.9.0") == 1);

    /* A pre-release sorts below its release, above the previous release. */
    CHECK(cmp("v0.7.0-rc.1", "v0.7.0") == -1);
    CHECK(cmp("v0.7.0", "v0.7.0-rc.1") == 1);
    CHECK(cmp("v0.7.0-rc.1", "v0.6.0") == 1);

    /* Pre-release identifier precedence (semver 2.0.0 section 11). */
    CHECK(cmp("v0.7.0-rc.2", "v0.7.0-rc.1") == 1);
    CHECK(cmp("v0.7.0-rc.10", "v0.7.0-rc.2") == 1);
    CHECK(cmp("v0.7.0-rc.1", "v0.7.0-beta.9") == 1);
    CHECK(cmp("v0.7.0-alpha", "v0.7.0-alpha.1") == -1);
    CHECK(cmp("v0.7.0-1", "v0.7.0-alpha") == -1);

    /* A dev build of a release is equal to it; of a pre-release equal too. */
    CHECK(cmp("v0.6.0-3-gabc1234", "v0.6.0") == 0);
    CHECK(cmp("v0.7.0-rc.1-3-gabc1234", "v0.7.0-rc.1") == 0);

    /* Remote tags are parsed strictly: "-dirty" and "-3-gabc" are pre-release
     * identifiers there, not decoration. */
    CHECK(strcmp(strict_pre_of("v0.7.0-rc.1-dirty"), "rc.1-dirty") == 0);
    CHECK(strcmp(strict_pre_of("v0.6.0-3-gabc1234"), "3-gabc1234") == 0);
    {
        semver_t a, b;
        CHECK(semver_parse("v0.7.0-rc.1", &a) && semver_parse("v0.7.0-rc.1-dirty", &b));
        CHECK(semver_cmp(&a, &b) < 0);
    }

    /* Numeric identifiers beyond any integer width still order correctly. */
    CHECK(cmp("v0.7.0-rc.9223372036854775809", "v0.7.0-rc.9223372036854775808") == 1);
    CHECK(cmp("v0.7.0-rc.2147483649", "v0.7.0-rc.2147483648") == 1);
    CHECK(cmp("v0.7.0-rc.0002", "v0.7.0-rc.2") == 0);

    /* Pre-release length boundary: SEMVER_PRE_LEN - 1 characters fit. */
    {
        char tag[80];
        semver_t v;
        snprintf(tag, sizeof(tag), "v1.0.0-%0*d", SEMVER_PRE_LEN - 1, 1);
        CHECK(semver_parse(tag, &v));
        snprintf(tag, sizeof(tag), "v1.0.0-%0*d", SEMVER_PRE_LEN, 1);
        CHECK(!semver_parse(tag, &v));
    }

    /* Best-tag selection across pages: the highest version on page two wins,
     * drafts of bad tags and overlong tags are skipped. */
    {
        semver_best_t best = { .have = false };
        semver_best_consider(&best, "v0.7.0-rc.1");   /* page 1 */
        semver_best_consider(&best, "not-a-version");
        semver_best_consider(&best, "v0.6.0");
        semver_best_consider(&best, "v0.7.0-rc.3");   /* page 2 */
        semver_best_consider(&best, "v0.7.0-rc.2");
        CHECK(best.have && strcmp(best.tag, "v0.7.0-rc.3") == 0);
        semver_best_consider(&best, "v0.7.0");
        CHECK(strcmp(best.tag, "v0.7.0") == 0);
        char longtag[80];
        snprintf(longtag, sizeof(longtag), "v9.0.0-%0*d", SEMVER_TAG_LEN, 1);
        semver_best_consider(&best, longtag);
        CHECK(strcmp(best.tag, "v0.7.0") == 0);
    }

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("semver: all tests passed\n");
    return 0;
}
