/* Semantic-version parsing and precedence for the OTA check. Pure C, no
 * ESP-IDF dependency, so it is covered by the host tests in firmware/test. */

#pragma once

#include <stdbool.h>

/* Longest accepted pre-release string. With the "vX.Y.Z-" prefix a tag still
 * fits SEMVER_TAG_LEN. Release tags are further capped at 31 characters by the
 * 32-byte esp_app_desc_t version the firmware embeds (see pages.yml). */
#define SEMVER_PRE_LEN 40

typedef struct {
    int major, minor, patch;
    /* Dot-separated pre-release identifiers ("rc.1"); empty for a release. */
    char pre[SEMVER_PRE_LEN];
} semver_t;

/* Strictly parses "v1.2.3", "1.2.3" or "1.2.3-rc.1", for release tags.
 * Build metadata ("+meta") is not supported. Returns false when the string is
 * not a version or the pre-release part is longer than SEMVER_PRE_LEN - 1. */
bool semver_parse(const char *s, semver_t *out);

/* Like semver_parse, for the version of the running firmware: `git describe`
 * decoration ("-<n>-g<sha>", "-dirty") is build metadata, not a pre-release,
 * and is dropped, so a dev build of v1.2.3 compares equal to v1.2.3. Never use
 * it for remote tags, where "-dirty" is a legitimate pre-release identifier. */
bool semver_parse_build(const char *s, semver_t *out);

/* Semver precedence: >0 if a > b, <0 if a < b, 0 if equal. A pre-release sorts
 * below its release (1.2.3-rc.1 < 1.2.3). */
int semver_cmp(const semver_t *a, const semver_t *b);

/* Longest release tag kept whole, including the NUL. */
#define SEMVER_TAG_LEN 48

/* Running maximum over release tags, e.g. across several API result pages. */
typedef struct {
    char tag[SEMVER_TAG_LEN];
    semver_t ver;
    bool have;
} semver_best_t;

/* Considers a remote release tag (strict parse). Tags that are not versions or
 * do not fit tag[] are skipped, never truncated: a clipped tag would build a
 * broken download URL. Equal versions keep the first one seen. */
void semver_best_consider(semver_best_t *best, const char *tag);
