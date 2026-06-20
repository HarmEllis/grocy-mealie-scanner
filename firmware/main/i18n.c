#include "i18n.h"

#include "lvgl.h"
#include <string.h>

#define TRANSLATIONS(X) \
    X("connected", "Connected", "Verbonden") \
    X("offline", "Offline", "Offline") \
    X("ready_to_scan", "Ready to scan", "Klaar om te scannen") \
    X("point_scanner", "Point the scanner\nat a barcode", "Richt de scanner\nop een barcode") \
    X("last", "LAST", "LAATSTE") \
    X("scanned", "Scanned", "Gescand") \
    X("in_stock", "IN STOCK", "VOORRAAD") \
    X("minimum", "MIN", "MIN") \
    X("opened_stat", "OPENED", "GEOPEND") \
    X("action_bought", "Bought", "Gekocht") \
    X("action_opened", "Opened", "Geopend") \
    X("action_consumed", "Consumed", "Verbruikt") \
    X("action_shopping", "Shopping", "Lijst") \
    X("saving_status", "Saving", "Opslaan") \
    X("saving", "Saving...", "Opslaan...") \
    X("added_to_list", "Added to list", "Aan lijst toegevoegd") \
    X("stock_change_fmt", "Stock  %s -> %s", "Voorraad  %s -> %s") \
    X("open_change_fmt", "Open  %s -> %s", "Geopend  %s -> %s") \
    X("on_shopping_list", "On shopping list", "Op boodschappenlijst") \
    X("error", "Error", "Fout") \
    X("tap_to_dismiss", "Tap to dismiss", "Tik om te sluiten") \
    X("unknown", "Unknown", "Onbekend") \
    X("product_not_found", "Product not found", "Product niet gevonden") \
    X("link_to_fmt", "Link to %s", "Koppel aan %s") \
    X("create_quoted_fmt", "Create \"%s\"", "Maak \"%s\" aan") \
    X("search_products", "Search products", "Producten zoeken") \
    X("scan_again", "Scan again", "Opnieuw scannen") \
    X("new_product", "New product", "Nieuw product") \
    X("create_in_grocy_as", "Create in Grocy as", "Maak in Grocy aan als") \
    X("create_product", "Create product", "Product aanmaken") \
    X("search", "Search", "Zoeken") \
    X("product_name_placeholder", "Product name...", "Productnaam...") \
    X("no_matches", "No matches", "Geen resultaten") \
    X("in_stock_fmt", "%s in stock", "%s op voorraad") \
    X("back", "Back", "Terug") \
    X("settings", "Settings", "Instellingen") \
    X("scan_feedback", "SCAN FEEDBACK", "SCANFEEDBACK") \
    X("scanner_beep", "Scanner beep", "Scannerpiep") \
    X("decode_tone", "GM67 decode tone", "GM67 decodeertoon") \
    X("status_light", "Status light", "Statuslamp") \
    X("result_flash", "WS2812 result flash", "WS2812 resultaat") \
    X("language", "Language", "Taal") \
    X("english", "English", "Engels") \
    X("dutch", "Dutch", "Nederlands") \
    X("changes_next_scan", "Changes apply on the next scan.", \
      "Wijzigingen gelden bij de volgende scan.") \
    X("screen_timeout", "Screen timeout", "Schermtime-out") \
    X("screen_timeout_never", "Never", "Nooit") \
    X("seconds_fmt", "%d s", "%d s") \
    X("touch_calibrate", "Calibrate touch", "Touch kalibreren") \
    X("scanner", "SCANNER", "SCANNER") \
    X("beep_off", "Off", "Uit") \
    X("beep_low", "Low", "Laag") \
    X("beep_med", "Medium", "Middel") \
    X("beep_high", "High", "Hoog") \
    X("scanner_light", "Scan light", "Scanlamp") \
    X("collimation", "Aim light", "Richtlamp") \
    X("on_scan", "On scan", "Bij scan") \
    X("always_off", "Always off", "Altijd uit") \
    X("touch_cal_instr", "Tap the centre of each target", \
      "Tik op het midden van elk doel") \
    X("touch_cal_done", "Touch calibration saved", "Touchkalibratie opgeslagen") \
    X("touch_cal_invalid", "Calibration failed. Tap all four corners.", \
      "Kalibratie mislukt. Tik op alle vier hoeken.") \
    X("touch_cal_save_failed", "Could not save touch calibration", \
      "Touchkalibratie kon niet worden opgeslagen") \
    X("setup", "Setup", "Instellen") \
    X("setup_info_fmt", "Scan to join  %s\npassword  %s\nthen open  http://192.168.4.1", \
      "Scan voor  %s\nwachtwoord  %s\nopen daarna  http://192.168.4.1") \
    X("looking_up", "Looking up...", "Opzoeken...") \
    X("starting_setup", "Starting setup...", "Instellen starten...") \
    X("connecting_wifi", "Connecting to WiFi...", "Verbinden met WiFi...") \
    X("portal_title", "Scanner setup", "Scanner instellen") \
    X("wifi_network", "WiFi network (SSID)", "WiFi-netwerk (SSID)") \
    X("wifi_password", "WiFi password", "WiFi-wachtwoord") \
    X("base_url", "grocy-mealie-sync base URL", "grocy-mealie-sync basis-URL") \
    X("device_token", "Device token", "Apparaattoken") \
    X("save_reboot", "Save and reboot", "Opslaan en herstarten") \
    X("saved", "Saved!", "Opgeslagen!") \
    X("rebooting", "The scanner reboots and connects to your WiFi.", \
      "De scanner herstart en maakt verbinding met je WiFi.") \
    X("form_too_large", "Form too large", "Formulier is te groot") \
    X("malformed_form", "Malformed form encoding", "Ongeldige formuliercodering") \
    X("required_fields", "SSID and base URL are required", \
      "SSID en basis-URL zijn verplicht") \
    X("url_too_long", "URL too long", "URL is te lang") \
    X("out_of_memory", "Out of memory", "Onvoldoende geheugen") \
    X("http_init_failed", "HTTP init failed", "HTTP initialiseren mislukt") \
    X("server_unreachable", "Server unreachable", "Server niet bereikbaar") \
    X("invalid_server_response", "Invalid server response", "Ongeldig serverantwoord") \
    X("invalid_device_token", "Invalid device token", "Ongeldig apparaattoken") \
    X("ping_failed", "Ping failed", "Verbindingstest mislukt") \
    X("lookup_failed", "Lookup failed", "Opzoeken mislukt") \
    X("action_failed", "Action failed", "Actie mislukt") \
    X("search_failed", "Search failed", "Zoeken mislukt") \
    X("create_failed", "Create failed", "Aanmaken mislukt") \
    X("link_failed", "Link failed", "Koppelen mislukt") \
    X("demo_unknown_product", "Demo: unknown product", "Demo: onbekend product") \
    X("demo_catalogue_full", "Demo: catalogue full", "Demo: catalogus is vol")

static const char *const s_languages[] = { "en", "nl", NULL };

#define TAG_ENTRY(tag, en, nl) tag,
static const char *const s_tags[] = {
    TRANSLATIONS(TAG_ENTRY)
    NULL,
};
#undef TAG_ENTRY

#define TEXT_ENTRY(tag, en, nl) en, nl,
static const char *const s_translations[] = {
    TRANSLATIONS(TEXT_ENTRY)
};
#undef TEXT_ENTRY

bool i18n_language_is_supported(const char *language)
{
    return language != NULL &&
           (strcmp(language, "en") == 0 || strcmp(language, "nl") == 0);
}

esp_err_t i18n_init(const char *language)
{
    if (lv_translation_add_static(s_languages, s_tags, s_translations) == NULL) {
        return ESP_ERR_NO_MEM;
    }
    i18n_set_language(language);
    return ESP_OK;
}

void i18n_set_language(const char *language)
{
    lv_translation_set_language(i18n_language_is_supported(language)
                                    ? language
                                    : I18N_DEFAULT_LANGUAGE);
}

const char *i18n_get_language(void)
{
    const char *language = lv_translation_get_language();
    return i18n_language_is_supported(language) ? language : I18N_DEFAULT_LANGUAGE;
}

const char *tr(const char *tag)
{
    return lv_tr(tag);
}

const char *i18n_tr_for(const char *tag, const char *language)
{
    size_t language_index = language != NULL && strcmp(language, "nl") == 0 ? 1 : 0;
    for (size_t i = 0; s_tags[i] != NULL; i++) {
        if (strcmp(s_tags[i], tag) == 0) {
            return s_translations[i * 2 + language_index];
        }
    }
    return tag;
}
