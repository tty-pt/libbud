#ifndef BUD_STATE_H
#define BUD_STATE_H

#include "bud.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BUD_BIN_MAGIC 0x42554431u /* 'B' 'U' 'D' '1' */
#define BUD_BIN_VERSION 1

typedef struct bud_bin_header {
	uint32_t magic;      /* BUD_BIN_MAGIC */
	uint16_t version;    /* BUD_BIN_VERSION */
	uint16_t flags;      /* Reserved, 0 */
	uint32_t struct_sz;  /* Expected struct size for ABI validation */
	uint32_t payload_sz; /* Size of binary payload following header */
} bud_bin_header_t;

/* --- Standalone Base64 Encoding / Decoding for Binary State Inlining --- */

static const char _bud_b64_table[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static inline size_t bud_b64_encode_len(size_t src_len)
{
	return ((src_len + 2) / 3) * 4 + 1;
}

static inline int bud_b64_encode(const void *src, size_t src_len, char *dst, size_t dst_cap)
{
	if (!src || !dst) return -1;
	size_t req = bud_b64_encode_len(src_len);
	if (dst_cap < req) return -1;

	const uint8_t *in = (const uint8_t *)src;
	char *out = dst;
	size_t i;

	for (i = 0; i + 2 < src_len; i += 3) {
		uint32_t n = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
		*out++ = _bud_b64_table[(n >> 18) & 63];
		*out++ = _bud_b64_table[(n >> 12) & 63];
		*out++ = _bud_b64_table[(n >> 6) & 63];
		*out++ = _bud_b64_table[n & 63];
	}
	if (i < src_len) {
		uint32_t n = (uint32_t)in[i] << 16;
		if (i + 1 < src_len) n |= (uint32_t)in[i + 1] << 8;
		*out++ = _bud_b64_table[(n >> 18) & 63];
		*out++ = _bud_b64_table[(n >> 12) & 63];
		if (i + 1 < src_len) {
			*out++ = _bud_b64_table[(n >> 6) & 63];
			*out++ = '=';
		} else {
			*out++ = '=';
			*out++ = '=';
		}
	}
	*out = '\0';
	return (int)(out - dst);
}

static inline int _bud_b64_val(char c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

static inline int bud_b64_decode(const char *src, size_t src_len, void *dst, size_t dst_cap)
{
	if (!src || !dst) return -1;
	const char *p = src;
	const char *end = src + src_len;
	uint8_t *out = (uint8_t *)dst;
	size_t written = 0;

	while (p < end) {
		while (p < end && (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t')) p++;
		if (p >= end) break;
		int v[4] = { -1, -1, -1, -1 };
		int count = 0;
		const char *token_start = p;
		while (p < end && count < 4) {
			if (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') { p++; continue; }
			if (*p == '=') { v[count++] = 0; p++; continue; }
			v[count] = _bud_b64_val(*p);
			if (v[count] < 0) return -1;
			count++;
			p++;
		}
		if (count < 2) break;
		if (written < dst_cap) out[written++] = (uint8_t)((v[0] << 2) | (v[1] >> 4));
		if (count > 2 && written < dst_cap && *(token_start + 2) != '=')
			out[written++] = (uint8_t)(((v[1] & 15) << 4) | (v[2] >> 2));
		if (count > 3 && written < dst_cap && *(token_start + 3) != '=')
			out[written++] = (uint8_t)(((v[2] & 3) << 6) | v[3]);
	}
	return (int)written;
}

static inline int bud_state_to_b64(const void *bin, size_t len, char *out, size_t cap)
{
	return bud_b64_encode(bin, len, out, cap);
}

static inline int bud_state_from_b64(const char *b64, size_t len, void *bin, size_t cap)
{
	return bud_b64_decode(b64, len, bin, cap);
}

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

#define BUD_STATE_SERIALIZE_BIN_FN(type_name, SCHEMA) \
	static inline int type_name##_serialize_bin(const type_name *st, void *buf, size_t cap) { \
		if (!st || !buf) return -1; \
		size_t total = sizeof(bud_bin_header_t) + sizeof(type_name); \
		if (cap < total) return -1; \
		bud_bin_header_t *hdr = (bud_bin_header_t *)buf; \
		hdr->magic = BUD_BIN_MAGIC; \
		hdr->version = BUD_BIN_VERSION; \
		hdr->flags = 0; \
		hdr->struct_sz = (uint32_t)sizeof(type_name); \
		hdr->payload_sz = (uint32_t)sizeof(type_name); \
		memcpy((char *)buf + sizeof(bud_bin_header_t), st, sizeof(type_name)); \
		return (int)total; \
	}

#define BUD_STATE_DESERIALIZE_BIN_FN(type_name, SCHEMA) \
	static inline int type_name##_deserialize_bin(type_name *st, const void *buf, size_t len) { \
		if (!st || !buf) return -1; \
		if (len < sizeof(bud_bin_header_t)) return -1; \
		const bud_bin_header_t *hdr = (const bud_bin_header_t *)buf; \
		if (hdr->magic != BUD_BIN_MAGIC) return -2; \
		if (hdr->version != BUD_BIN_VERSION) return -3; \
		if (hdr->struct_sz != sizeof(type_name)) return -4; \
		if (len < sizeof(bud_bin_header_t) + hdr->payload_sz) return -5; \
		memcpy(st, (const char *)buf + sizeof(bud_bin_header_t), sizeof(type_name)); \
		return 0; \
	}

#define BUD_STATE_SERIALIZE_B64_FN(type_name, SCHEMA) \
	static inline int type_name##_serialize_b64(const type_name *st, char *out_b64, size_t cap) { \
		char bin_buf[sizeof(bud_bin_header_t) + sizeof(type_name)]; \
		int n = type_name##_serialize_bin(st, bin_buf, sizeof(bin_buf)); \
		if (n <= 0) return -1; \
		return bud_b64_encode(bin_buf, (size_t)n, out_b64, cap); \
	}

#define BUD_STATE_DESERIALIZE_B64_FN(type_name, SCHEMA) \
	static inline int type_name##_deserialize_b64(type_name *st, const char *b64, size_t len) { \
		char bin_buf[sizeof(bud_bin_header_t) + sizeof(type_name) + 16]; \
		int n = bud_b64_decode(b64, len, bin_buf, sizeof(bin_buf)); \
		if (n <= 0) return -1; \
		return type_name##_deserialize_bin(st, bin_buf, (size_t)n); \
	}

#define BUD_DEFINE_STATE(type_name, SCHEMA) \
	BUD_STATE_STRUCT(type_name, SCHEMA) \
	BUD_STATE_SERIALIZE_FN(type_name, SCHEMA) \
	BUD_STATE_DESERIALIZE_FN(type_name, SCHEMA) \
	BUD_STATE_SERIALIZE_BIN_FN(type_name, SCHEMA) \
	BUD_STATE_DESERIALIZE_BIN_FN(type_name, SCHEMA) \
	BUD_STATE_SERIALIZE_B64_FN(type_name, SCHEMA) \
	BUD_STATE_DESERIALIZE_B64_FN(type_name, SCHEMA)

#ifdef __cplusplus
}
#endif

#endif /* BUD_STATE_H */
