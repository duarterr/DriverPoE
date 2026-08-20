/*
 * Device identity: deterministic serial and derived admin key, with
 * two-phase key rotation (staging + confirmation) and a remote CLAIM path
 * for provisioning with no serial/debugger connection.
 *
 * The serial is NEVER written to flash — it's recomputed every boot from
 * the eFuse base MAC (esp_efuse_mac_get_default()) + DEVID_MODEL_PREFIX
 * (poe_luminaire.h). The key (32 bytes) and the epoch (uint32) are written to
 * the "idnvs" partition, namespace "devid" — completely separate from the
 * app's "nvs" partition (the one hv9910.c uses for brightness). Two
 * ways to get them there: (a) tools/lumtool.py writing "idnvs" directly
 * over serial (esptool.py), or (b) the admin channel's CLAIM command,
 * entirely over the network, unauthenticated (there's nothing to
 * authenticate against yet -- see devid_claim() below and
 * admin_channel.c's handle_claim()). The admin channel's
 * FACTORY_RESET only erases "nvs" — "idnvs" is never touched, so
 * serial/key/epoch always survive.
 *
 * Key derivation (HKDF-SHA256, ikm=master secret) does NOT happen in the
 * firmware — only on the production station and on the admin tool
 * (Python). The device only ever stores the already-derived result. See
 * README.md ("Identidade do dispositivo") for the full formula.
 *
 * No flash encryption / NVS encryption in this product (see README.md) —
 * physical access to a unit reveals THAT unit's key, but never the master
 * secret nor any other unit's key.
 *
 * Fail-closed: if no key/epoch was ever written, devid_is_provisioned()
 * returns false and admin_channel.c refuses any authenticated
 * command (only DISCOVER, which needs no authentication, keeps working).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVID_KEY_LEN     32
#define DEVID_NONCE_LEN   16  /* must match ADMIN_NONCE_LEN in admin_protocol.h */

/* Opens the "idnvs" partition and loads key/epoch into RAM. Call early in
 * app_main(), independent of everything else (doesn't depend on PoE,
 * network, etc). */
void devid_init(void);

/* true if key+epoch were successfully read from the "idnvs" partition. */
bool devid_is_provisioned(void);

/* ASCII serial, e.g. "LUM1-A4CF12B93D08" — computed once (internally
 * cached), never written to flash. Always available, even without
 * provisioning (only depends on the eFuse). */
const char *devid_get_serial(void);

/* Pointer to the 32 bytes of the currently ACTIVE key. Only valid if
 * devid_is_provisioned() == true. Never NULL in that case. */
const uint8_t *devid_get_key(void);

/* Epoch of the active key. */
uint32_t devid_get_epoch(void);

/* --- Remote claim (CLAIM), for provisioning with no serial/debugger ---
 *
 * CLAIM is UNAUTHENTICATED, the same as DISCOVER -- see
 * admin_channel.c's handle_packet(). There's no secret guarding it
 * because there's nothing to guard yet: a unit only accepts CLAIM while
 * devid_is_provisioned() == false, i.e. before it has a real identity at
 * all. The moment devid_claim() below succeeds, CLAIM stops being
 * accepted on that unit forever -- it can only ever set the FIRST
 * identity, never overwrite one. */

/* Writes (new_key, new_epoch) to "idnvs" and makes it the active identity
 * immediately -- no staging/confirmation needed here (unlike
 * devid_rotate_*): there is no existing identity that could be lost, so a
 * failed or lost CLAIM just leaves the unit unprovisioned, safe to retry.
 * Refuses (returns false, touches nothing) if the unit is already
 * provisioned -- CLAIM can only ever set the FIRST identity, never
 * overwrite one; see admin_channel.c's handle_claim(). */
bool devid_claim(const uint8_t new_key[DEVID_KEY_LEN], uint32_t new_epoch);

/* --- Key rotation (ROTATE_KEY / ROTATE_CONFIRM) ---
 *
 * Flow: devid_rotate_stage() keeps the new key in RAM only (writes
 * nothing yet). devid_rotate_commit() is what actually takes effect: it
 * writes the staged key to "idnvs" and it becomes the active key. If
 * devid_rotate_commit() is never called within DEVID_ROTATE_TTL_S
 * seconds, the staged key expires on its own
 * (devid_rotate_has_staged() starts returning false) and the active key
 * never changes — safe rollback, no external intervention needed.
 */

/* Stores (new_key, new_epoch) as staged, replacing any previous staging.
 * 'nonce' is the nonce of the ROTATE_KEY packet that originated this
 * staging — the matching ROTATE_CONFIRM must present this SAME nonce
 * (not a fresh one obtained via CHALLENGE), binding the confirmation to
 * this specific rotation request. Doesn't touch the active key or flash. */
void devid_rotate_stage(const uint8_t new_key[DEVID_KEY_LEN], uint32_t new_epoch,
                         const uint8_t nonce[DEVID_NONCE_LEN]);

/* true if a staged key still exists and hasn't expired. */
bool devid_rotate_has_staged(void);

/* Pointer to the 32 bytes of the staged key / its associated nonce. Only
 * valid if devid_rotate_has_staged() == true. */
const uint8_t *devid_rotate_get_staged_key(void);
const uint8_t *devid_rotate_get_staged_nonce(void);
uint32_t devid_rotate_get_staged_epoch(void);

/* Writes the staged key to "idnvs" and promotes it to the active key.
 * Does nothing (returns false) if there's no valid staging. */
bool devid_rotate_commit(void);

/* Discards the staged key without committing anything. Called
 * automatically when the TTL expires, but also exposed for explicit use. */
void devid_rotate_discard(void);

#ifdef __cplusplus
}
#endif
