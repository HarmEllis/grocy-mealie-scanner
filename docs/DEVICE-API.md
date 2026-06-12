# Device API contract (v1)

The scanner talks to a device-facing REST API hosted by
[grocy-mealie-sync](https://github.com/HarmEllis/grocy-mealie-sync) under
`/api/device/v1/*`. This document is the single source of truth for the
contract; the implementation lives in grocy-mealie-sync
(`src/app/api/device/v1/**`), the consumer is this repo's firmware
(`firmware/main/api_client.c`).

## Authentication

Every request carries `Authorization: Bearer <token>`.

- Tokens are configured in grocy-mealie-sync via the `DEVICE_API_TOKENS`
  environment variable (comma-separated, one per device; trimmed, empty
  entries ignored).
- Device tokens are only valid for `/api/device/*` paths. The admin
  `AUTH_SECRET` keeps working on those paths too (useful for testing), but
  device tokens must never be accepted on non-device routes.
- Missing/invalid token → `401 {"error":"Unauthorized"}`.
- When `DEVICE_API_TOKENS` is unset and auth is enabled, device routes only
  accept `AUTH_SECRET`. When auth is disabled entirely (`AUTH_ENABLED=false`),
  device routes are open like the rest of the API.

All requests and responses are JSON (`content-type: application/json`).
Errors use the existing repo shape: `{"error": "<message>"}` with an
appropriate 4xx/5xx status.

## Endpoints

### `GET /api/device/v1/ping`

Connectivity + auth check used by the firmware at boot and as a health probe.

```json
{ "ok": true, "app": "grocy-mealie-sync", "version": "1.9.0" }
```

### `GET /api/device/v1/scan/{barcode}`

Look up a scanned barcode. Exactly one of the two response variants:

**Known barcode** (`200`):

```json
{
  "status": "found",
  "product": {
    "id": 42,
    "name": "Heinz Tomato Ketchup",
    "quantityUnit": "Bottle",
    "stockAmount": 3,
    "openedAmount": 1,
    "minStockAmount": 2,
    "onShoppingList": false
  }
}
```

**Unknown barcode** (`200`, not an error — the device shows the not-found
flow):

```json
{
  "status": "unknown",
  "barcode": "8715700110622",
  "externalLookup": {
    "source": "openfoodfacts",
    "name": "Tomato Ketchup",
    "brand": "Heinz",
    "quantity": "570 g"
  },
  "suggestedMatches": [
    { "id": 42, "name": "Heinz Tomato Ketchup", "score": 0.91 }
  ]
}
```

- `externalLookup` is `null` when OpenFoodFacts has no result (or the lookup
  failed/timed out — lookups are capped at 3 s so the device never hangs).
- `suggestedMatches` fuzzy-matches the external name (when present) and the
  raw barcode against all Grocy products via the existing `fuzzy-match`
  helpers; at most 5 entries, best first, score in `[0,1]`.
- Invalid barcode (non `[0-9A-Za-z_-]{4,64}`) → `400`.

### `POST /api/device/v1/products/{id}/action`

Execute one of the four tile actions on a known product.

Request:

```json
{ "action": "purchase", "amount": 1 }
```

- `action`: `"purchase" | "open" | "consume" | "add_to_shopping_list"`
- `amount`: optional, defaults to `1`, must be a positive number.

Response (`200`) always reports before/after so the firmware can render the
flash pill ("Stock 3 → 4") with real numbers:

```json
{
  "ok": true,
  "action": "purchase",
  "product": { "id": 42, "name": "Heinz Tomato Ketchup" },
  "stock":  { "before": 3, "after": 4 },
  "opened": { "before": 1, "after": 1 },
  "shoppingList": null
}
```

For `add_to_shopping_list`, `stock`/`opened` are unchanged and
`shoppingList` reports `{ "itemId": "…", "quantity": 2 }` (quantity after
adding; adding an already-listed product increments it).

Semantics (mapped onto existing grocy-mealie-sync use-cases):

| action | effect |
|---|---|
| `purchase` | Grocy stock add (purchase) of `amount` |
| `open` | Grocy mark-opened of `amount` |
| `consume` | Grocy consume of `amount` (opened stock first) |
| `add_to_shopping_list` | add/increment the mapped item on the Mealie shopping list |

Failure cases: unknown product id → `404`; `consume`/`open` with
insufficient stock → `409 {"error":"Not enough in stock", "stock":{...}}`
(device shows an error toast, no flash).

### `GET /api/device/v1/products?query=<q>&limit=<n>`

Product search for the on-device "link/search" screen. Fuzzy search over all
Grocy products; `limit` defaults to 8, max 25.

```json
{
  "results": [
    { "id": 42, "name": "Heinz Tomato Ketchup", "stockAmount": 3 }
  ]
}
```

Empty `query` → `400`.

### `POST /api/device/v1/products`

Create a new Grocy product from the device's confirmed proposal (name may
have been edited with the on-screen keyboard) and link the barcode to it.

Request:

```json
{ "name": "Tomato Ketchup", "barcode": "8715700110622" }
```

- Created with the instance's default quantity unit and location (server
  settings; see grocy-mealie-sync settings for device defaults).
- Duplicate name (case-insensitive exact match) → `409` with the existing
  product, so the device can offer to link instead:
  `{"error":"Product already exists","product":{"id":42,"name":"…"}}`.

Response (`201`): the `product` object **at the response root** (not wrapped
in a `{"product": …}` envelope, unlike `scan`) — same field shape as the
`product` inside the `scan` "found" variant, with stock numbers all 0:

```json
{ "id": 99, "name": "Tomato Ketchup", "quantityUnit": "Piece",
  "stockAmount": 0, "openedAmount": 0, "minStockAmount": 0,
  "onShoppingList": false }
```

### `POST /api/device/v1/products/{id}/barcodes`

Link a barcode to an existing product (the "link to suggested match" path).

Request: `{ "barcode": "8715700110622" }`

Response (`200`): the full `product` object **at the response root** (same
unwrapped shape as `POST /products` above, not the `scan` envelope) so the
device can transition straight to the product screen.

Barcode already linked to another product → `409` with that product wrapped
in the error body: `{"error":"…","product":{"id":42,"name":"…"}}`.

## Firmware-driven design constraints

- Responses must stay small (< 2 KB) and flat; the firmware parses with cJSON
  on a memory-constrained device.
- Every state the UI can land in after a network call must be derivable from
  a single response (no follow-up requests needed to render).
- All endpoints must respond within 5 s (the firmware request timeout is
  8 s); external lookups are capped server-side at 3 s.
- The device does not auto-retry: every request (GET or POST) is sent exactly
  once and the user re-taps on failure, so duplicate-action protection
  server-side is not required for v1.
