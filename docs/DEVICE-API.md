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
{ "ok": true, "app": "grocy-mealie-sync", "version": "1.9.0", "apiVersion": 4 }
```

- `apiVersion` is the device-API capability version. The firmware defaults to
  `1` when the field is absent (older servers) and uses it to hide features the
  server does not yet support — e.g. the on-device product-search affordance is
  shown only when `apiVersion >= 2` (adds `GET /products/{id}`).
  - `apiVersion >= 3` advertises `shoppingListAmount` on device products
    (the shopping-list count card). The field is best-effort and defaults to
    `0`, so the card degrades gracefully on a pre-`3` server rather than 404ing.
  - `apiVersion >= 4` advertises `GET /labels`, `GET /letters`, and the
    `labelId`/`letter` filters on `GET /products` (the home-screen "Browse"
    button: browse by label / by A-Z).

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
    "onShoppingList": false,
    "shoppingListAmount": 0
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
- Grocy barcode lookup is capped server-side below the firmware HTTP timeout;
  an upstream timeout returns `504 {"error":"Grocy barcode lookup timed out after 6500ms"}`.
- `onShoppingList` and `shoppingListAmount` are best-effort and come from one
  Mealie shopping-list check. Slow or unreachable checks must fall back to
  `false` / `0` instead of delaying the product screen. `shoppingListAmount` is
  the summed quantity of the matching unchecked rows; `onShoppingList` stays
  independent of it (a quantity-0 row is still on the list, so it can be `true`
  while `shoppingListAmount` is `0`).
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

Empty `query` → `400`. A duplicate parameter key (e.g. `?query=a&query=b`), an
unknown parameter, or `offset` combined with `query` (not part of this
variant's contract, only the `labelId`/`letter` variants below) → `400`.

### `GET /api/device/v1/products?labelId=<id>&offset=<n>&limit=<n>`

### `GET /api/device/v1/products?letter=<A-Z|#>&offset=<n>&limit=<n>`

Requires `apiVersion >= 4`. Backs the home-screen browse flow's
product lists: all products carrying a Mealie label, or all products whose
name starts with a given letter.

Exactly one of `query`, `labelId`, `letter` is required — `0` or `2+` present,
or any other query parameter present, → `400`. `offset` defaults to `0` (must
be a non-negative integer); `limit` defaults to `20`, **max `20`** (matches the
firmware's fixed-size result array — unlike the `query` variant above, which
allows up to 25). `letter` must be exactly one uppercase ASCII letter, or the
literal `#` (products whose normalized name doesn't start with `a`-`z` — a
digit, symbol, or non-Latin name). `labelId` is either a real Mealie label id
(see `GET /labels` below) or the reserved sentinel `unlabeled`.

```json
{
  "results": [
    { "id": 42, "name": "Heinz Tomato Ketchup", "stockAmount": 3 }
  ],
  "total": 3,
  "offset": 0,
  "hasMore": false
}
```

v1 firmware requests only `offset=0` (no infinite scroll); when `total` is
greater than the number of `results` returned, it shows a static "+N more"
hint instead of silently truncating the bucket. Building the label/letter
index snapshot (Grocy products/stock + all Mealie foods/labels) is capped at
a 6s internal deadline, comfortably inside the firmware's 8s HTTP timeout; an
exceeded deadline → `504`.

### `GET /api/device/v1/labels`

Requires `apiVersion >= 4`. Lists Mealie labels with at least one mapped
Grocy product, for the "Browse → By label" picker.

```json
{
  "labels": [
    { "id": "3f2504e0-4f89-11d3-9a0c-0305e82c3301", "name": "Broodbeleg", "count": 5 },
    { "id": "unlabeled", "name": "Overig", "count": 12 }
  ]
}
```

- Not paginated. Real labels are ranked by product count and capped at the
  top **19**; `unlabeled` (the reserved sentinel for products with no
  resolvable Mealie label — no mapping, a stale mapping, or a food with no
  label) is **outside that cap**, always appended last when its count is
  `> 0`, so products without a label always have a label-browse entry.
  v1 limitations (documented, not bugs): a household with more than 19
  labels does not see its least-used labels here, so products whose labels
  all fall outside the top 19 have no label-browse entry; and every browse
  list shows only its first page (20 products, see `total`). Such products
  are still reachable via text search (and via A-Z when they are within the
  first 20 of their letter). Label and product names are truncated
  server-side to a UTF-8-safe 63-byte cap (matching the firmware's
  `API_NAME_LEN`). That cap alone does not bound the response size (JSON
  escaping can grow a byte to six), so the server must also enforce the
  serialized-size budget below.
- Display order (after the count-based selection above): label name
  ascending, case/diacritic-insensitive; `unlabeled` always last.

### `GET /api/device/v1/letters`

Requires `apiVersion >= 4`. Lists all 27 A-Z + `#` buckets with their product
counts, for the "Browse → A-Z" grid (buckets with `count: 0` are shown
dimmed/disabled by the firmware, not omitted).

```json
{
  "letters": [
    { "letter": "A", "count": 4 },
    { "letter": "B", "count": 0 },
    "...",
    { "letter": "#", "count": 1 }
  ]
}
```

### `GET /api/device/v1/products/{id}`

Fetch one product by id. Backs the home-screen **search → pick** flow: the
device opens product search from idle (no barcode in flight), the user picks a
result, and the device shows that product exactly as if it had been scanned —
without linking any barcode. Requires `apiVersion >= 2` (see `/ping`).

Response (`200`): the **unwrapped** product object at the response root (same
shape as `POST /products` and `POST /products/{id}/barcodes`, not the `scan`
envelope):

```json
{ "id": 42, "name": "Heinz Tomato Ketchup", "quantityUnit": "Bottle",
  "stockAmount": 3, "openedAmount": 1, "minStockAmount": 2,
  "onShoppingList": false, "shoppingListAmount": 0 }
```

Invalid id (non-positive / non-integer) → `400`; unknown product → `404`.

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
  "onShoppingList": false, "shoppingListAmount": 0 }
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
  on a memory-constrained device. The `/labels`, `/letters`, and browse
  variants of `GET /products` are capped (19+1 labels, 20 products, 27
  letters), but the hard limit is on the serialized body: every response,
  measured as encoded UTF-8 JSON bytes including escaping, must stay under
  the firmware's 4096-byte `MAX_RESPONSE` buffer (larger bodies are cut off
  and fail to parse). The server must enforce this, e.g. by stripping
  control characters from names before the 63-byte truncation or by
  budgeting names by their encoded size, and its contract tests must cover
  escape-heavy and multi-byte names. 2 KB remains the target for realistic
  households; 4096 is the ceiling that must never be crossed.
- Every state the UI can land in after a network call must be derivable from
  a single response (no follow-up requests needed to render).
- Endpoints must respond before the firmware request timeout (8 s). The scan
  path caps Grocy barcode lookup at 6.5 s, OpenFoodFacts lookup at 3 s, and
  Mealie shopping-list status at 1.2 s.
- The device may retry fast transport failures for scan lookups only. Action
  requests are sent exactly once and the user re-taps on failure, so
  duplicate-action protection server-side is not required for v1.
