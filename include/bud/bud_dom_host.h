#ifndef BUD_DOM_HOST_H
#define BUD_DOM_HOST_H

/**
 * @file bud_dom_host.h
 * @brief Generic Browser DOM host_ops adapter for bud_reconcile (WASM & native).
 *
 * Bridges bud_reconcile.h directly onto browser DOM nodes via opaque integer
 * handles (handle 1 = root container) and thin WASM host imports in bud-cap.js.
 */

#include "bud_reconcile.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Root container handle reserved by the DOM host (handle 1). */
#define BUD_DOM_ROOT_HANDLE ((void *)(uintptr_t)1u)

/**
 * @brief Return a bud_host_ops vtable backed by browser DOM host imports.
 *
 * On WASM (__wasm__), each callback invokes the corresponding `bud` module
 * import (`bud_dom_create_el`, etc.). On native builds, stubs are provided
 * for link compatibility (or unit testing via bud_dom_dispatch).
 */
bud_host_ops bud_dom_host_ops(void);

/**
 * @brief Dispatch a DOM event from JS into a registered C bud_event_handler_fn.
 *
 * @param[in] handler_id  Integer listener handle returned to JS by set_event.
 * @param[in] payload     Optional UTF-8 event payload (e.g. input value / key),
 *                        passed to handler via ev.user if listener ud is NULL,
 *                        or retrieved via bud_dom_event_payload().
 * @param[in] payload_len Byte length of payload.
 * @return Return value of the C handler (non-zero if stopped), or 0 if invalid.
 */
int bud_dom_dispatch(uint32_t handler_id, const char *payload, size_t payload_len);

/**
 * @brief Return the current DOM event payload string during a bud_dom_dispatch call.
 *
 * Allows lifted .cx closures (which already occupy ev->user with their capture
 * struct) to also read the DOM event payload (input value, key name, etc.).
 */
const char *bud_dom_event_payload(void);

/**
 * @brief Set the active event payload string for the current dispatch frame.
 * @return Previous payload pointer (pass back to restore).
 */
const char *bud_dom_set_event_payload(const char *payload);

/**
 * @brief Reset the internal event-listener registry (e.g. on unmount/test reset).
 */
void bud_dom_host_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* BUD_DOM_HOST_H */
