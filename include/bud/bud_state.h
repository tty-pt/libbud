#ifndef BUD_STATE_H
#define BUD_STATE_H

#include "bud.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Declarative state schema macro helpers for C-isomorphic web applications.
 *
 * Example usage:
 *
 * #define CHROME_STATE_SCHEMA(F, st) \
 *     F(STR,  st, title, 256, "")     \
 *     F(STR,  st, path,  256, "")     \
 *     F(STR,  st, icon,  64,  "")     \
 *     F(STR,  st, user,  64,  "")     \
 *     F(STR,  st, lang,  32,  "en")
 *
 * BUD_DEFINE_STATE(site_ui_chrome_state, CHROME_STATE_SCHEMA)
 *
 * This generates:
 * 1. typedef struct site_ui_chrome_state { ... } site_ui_chrome_state;
 * 2. int site_ui_chrome_state_serialize(const site_ui_chrome_state *st, char *buf, size_t cap);
 * 3. int site_ui_chrome_state_deserialize(site_ui_chrome_state *st, const char *json, size_t len);
 */

#define _BUD_STRUCT_STR(st, name, sz, def) char name[sz];
#define _BUD_STRUCT_INT(st, name, sz, def) int name;
#define _BUD_STRUCT_UINT(st, name, sz, def) unsigned int name;
#define _BUD_STRUCT_BOOL(st, name, sz, def) int name;

#define _BUD_SER_STR(st, name, sz, def) \
	do { \
		if ((st)->name[0]) { \
			int _n = snprintf(p, (size_t)(end - p), "%s\"" #name "\":\"%s\"", _first ? "" : ",", (st)->name); \
			if (_n > 0) { p += _n; _first = 0; } \
		} \
	} while (0);

#define _BUD_SER_INT(st, name, sz, def) \
	do { \
		int _n = snprintf(p, (size_t)(end - p), "%s\"" #name "\":%d", _first ? "" : ",", (st)->name); \
		if (_n > 0) { p += _n; _first = 0; } \
	} while (0);

#define _BUD_SER_UINT(st, name, sz, def) \
	do { \
		int _n = snprintf(p, (size_t)(end - p), "%s\"" #name "\":%u", _first ? "" : ",", (st)->name); \
		if (_n > 0) { p += _n; _first = 0; } \
	} while (0);

#define _BUD_SER_BOOL(st, name, sz, def) \
	do { \
		int _n = snprintf(p, (size_t)(end - p), "%s\"" #name "\":%s", _first ? "" : ",", (st)->name ? "true" : "false"); \
		if (_n > 0) { p += _n; _first = 0; } \
	} while (0);

#define _BUD_DESER_STR(st, name, sz, def) \
	bud_json_str_len(json, len, #name, (st)->name, sizeof((st)->name));

#define _BUD_DESER_INT(st, name, sz, def) \
	(st)->name = bud_json_int_len(json, len, #name, def);

#define _BUD_DESER_UINT(st, name, sz, def) \
	(st)->name = (unsigned int)bud_json_int_len(json, len, #name, def);

#define _BUD_DESER_BOOL(st, name, sz, def) \
	(st)->name = bud_json_int_len(json, len, #name, def) ? 1 : 0;

#define _BUD_FIELD_DISPATCH(KIND, st, name, sz, def) \
	_BUD_STRUCT_##KIND(st, name, sz, def)

#define BUD_STATE_STRUCT(type_name, SCHEMA) \
	typedef struct type_name { \
		SCHEMA(_BUD_FIELD_DISPATCH, type_name) \
	} type_name;

#define _BUD_SER_DISPATCH(KIND, st, name, sz, def) \
	_BUD_SER_##KIND(st, name, sz, def)

#define BUD_STATE_SERIALIZE_FN(type_name, SCHEMA) \
	static inline int type_name##_serialize(const type_name *st, char *buf, size_t cap) { \
		if (!st || !buf || cap < 3) return -1; \
		char *p = buf; \
		char *end = buf + cap; \
		*p++ = '{'; \
		int _first = 1; \
		SCHEMA(_BUD_SER_DISPATCH, st) \
		if (p < end - 1) { \
			*p++ = '}'; \
			*p = '\0'; \
			return (int)(p - buf); \
		} \
		buf[cap - 1] = '\0'; \
		return -1; \
	}

#define _BUD_DESER_DISPATCH(KIND, st, name, sz, def) \
	_BUD_DESER_##KIND(st, name, sz, def)

#define BUD_STATE_DESERIALIZE_FN(type_name, SCHEMA) \
	static inline int type_name##_deserialize(type_name *st, const char *json, size_t len) { \
		if (!st || !json) return -1; \
		SCHEMA(_BUD_DESER_DISPATCH, st) \
		return 0; \
	}

#define BUD_DEFINE_STATE(type_name, SCHEMA) \
	BUD_STATE_STRUCT(type_name, SCHEMA) \
	BUD_STATE_SERIALIZE_FN(type_name, SCHEMA) \
	BUD_STATE_DESERIALIZE_FN(type_name, SCHEMA)

#ifdef __cplusplus
}
#endif

#endif /* BUD_STATE_H */
