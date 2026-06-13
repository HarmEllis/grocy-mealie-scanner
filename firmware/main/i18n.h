#pragma once

#include "esp_err.h"
#include <stdbool.h>

#define I18N_LANGUAGE_LEN 3
#define I18N_DEFAULT_LANGUAGE "en"

/* Registers the static English/Dutch catalog and selects `language`. */
esp_err_t i18n_init(const char *language);

/* Selects a supported BCP 47 language code ("en" or "nl"). */
void i18n_set_language(const char *language);

const char *i18n_get_language(void);
bool i18n_language_is_supported(const char *language);

/* Returns the selected translation for `tag`. */
const char *tr(const char *tag);

/* Returns a translation without changing LVGL's active language. */
const char *i18n_tr_for(const char *tag, const char *language);
