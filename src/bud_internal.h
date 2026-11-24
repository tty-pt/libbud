#ifndef BUD_INTERNAL_H
#define BUD_INTERNAL_H

#include "../include/bud/bud_jsx.h"
#include "../include/bud/bud.h"
#include "../include/bud/bud_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>

#if defined(__wasm__) || defined(__STDC_NO_THREADS__)
#define BUD_TLS
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define BUD_TLS _Thread_local
#elif defined(__GNUC__) || defined(__clang__)
#define BUD_TLS __thread
#else
#define BUD_TLS
#endif

#define BUD_ARENA_DEFAULT_CAP      8192u
#define BUD_ARENA_MAX_SLAB_CAP     65536u
#define BUD_ARENA_MAX_RETAIN_BYTES 262144u

typedef struct bud_arena_slab {
	struct bud_arena_slab *next;
	size_t used;
	size_t cap;
	max_align_t data[];
} bud_arena_slab;

struct bud_arena {
	bud_arena_slab *head;
	bud_arena_slab *cur;
	size_t initial_cap;
	size_t total_used;
	size_t slab_count;
};

extern BUD_TLS bud_arena *bud_active_arena;

void *bud_arena_alloc_slow(bud_arena *arena, size_t size, size_t align);

static inline size_t bud_align_up(size_t n, size_t align)
{
	return (n + (align - 1u)) & ~(align - 1u);
}

static inline void *bud_arena_alloc(bud_arena *arena, size_t size, size_t align)
{
	bud_arena_slab *s = arena->cur;
	size_t off = bud_align_up(s->used, align);
	if (off <= s->cap && size <= s->cap - off) {
		unsigned char *base = (unsigned char *)s->data;
		s->used = off + size;
		arena->total_used += size;
		return base + off;
	}
	return bud_arena_alloc_slow(arena, size, align);
}

enum {
	BUD_ATTR_ARENA      = 1u << 0,
	BUD_ATTR_NAME_OWNED = 1u << 1,
	BUD_ATTR_VAL_OWNED  = 1u << 2
};

enum {
	BUD_LISTENER_ARENA       = 1u << 0,
	BUD_LISTENER_EVENT_OWNED = 1u << 1,
	BUD_LISTENER_USER_OWNED  = 1u << 2
};

enum {
	BUD_NODE_ARENA       = 1u << 0,
	BUD_NODE_TAG_OWNED   = 1u << 1,
	BUD_NODE_TEXT_OWNED  = 1u << 2,
	BUD_NODE_KEY_OWNED   = 1u << 3,
	BUD_NODE_PROPS_OWNED = 1u << 4
};

typedef struct bud_attr bud_attr;
typedef struct bud_listener bud_listener;

struct bud_attr {
	const char *name;
	const char *value;
	bud_attr *next;
	uint8_t is_boolean;
	uint8_t flags;
};

struct bud_listener {
	const char *event;
	bud_event_handler_fn handler;
	void *user;
	size_t user_size;
	bud_listener *next;
	uint8_t bubbles;
	uint8_t flags;
};

struct bud_node {
	bud_node_kind kind;
	unsigned int id;
	const char *tag;
	const char *text;
	const char *key;
	bud_component_fn comp_fn;
	void *comp_ctx;
	void *comp_props;
	size_t comp_props_size;
	bud_attr *attrs;
	bud_listener *listeners;
	bud_node *parent;
	bud_node *first_child;
	bud_node *last_child;
	bud_node *next_sibling;
	bud_lifecycle_fn on_mount;
	bud_lifecycle_fn on_update;
	bud_lifecycle_fn on_unmount;
	void *lifecycle_user;
	uint8_t mounted;
	uint8_t ids_frozen;
	uint8_t flags;
	BUD_SRC_FIELDS
};

typedef struct bud_runtime {
	bud_node *root;
	bud_runtime_invalidate_fn invalidate;
	void *invalidate_user;
	int dirty;
	int mounted;
} bud_runtime;

typedef struct bud_buf {
	char *data;
	size_t len;
	size_t cap;
} bud_buf;

static inline char *bud_strdup(const char *value)
{
	size_t len;
	char *copy;

	if (!value) {
		return NULL;
	}

	len = strlen(value);
	copy = (char *)malloc(len + 1);
	if (!copy) {
		return NULL;
	}

	memcpy(copy, value, len + 1);
	return copy;
}

static inline void *bud_alloc_zero(size_t size, size_t align, int *from_arena)
{
	bud_arena *arena = bud_active_arena;
	if (arena) {
		void *p = bud_arena_alloc(arena, size, align);
		if (p) {
			memset(p, 0, size);
			*from_arena = 1;
		}
		return p;
	}
	*from_arena = 0;
	return calloc(1, size);
}

static inline const char *bud_alloc_str(const char *value, int *owned)
{
	size_t len;
	char *copy;
	bud_arena *arena;

	if (!value) {
		*owned = 0;
		return NULL;
	}

	len = strlen(value);
	arena = bud_active_arena;
	if (arena) {
		copy = (char *)bud_arena_alloc(arena, len + 1, 1);
		if (!copy) {
			*owned = 0;
			return NULL;
		}
		memcpy(copy, value, len + 1);
		*owned = 0;
		return copy;
	}

	copy = (char *)malloc(len + 1);
	if (!copy) {
		*owned = 0;
		return NULL;
	}
	memcpy(copy, value, len + 1);
	*owned = 1;
	return copy;
}

static inline void *bud_alloc_bytes(const void *src, size_t size, int *owned)
{
	void *copy;
	bud_arena *arena = bud_active_arena;

	if (arena) {
		copy = bud_arena_alloc(arena, size, _Alignof(max_align_t));
		if (!copy) {
			*owned = 0;
			return NULL;
		}
		memcpy(copy, src, size);
		*owned = 0;
		return copy;
	}

	copy = malloc(size);
	if (!copy) {
		*owned = 0;
		return NULL;
	}
	memcpy(copy, src, size);
	*owned = 1;
	return copy;
}

static inline void bud_buf_init(bud_buf *buf)
{
	buf->data = NULL;
	buf->len = 0;
	buf->cap = 0;
}

static inline int bud_buf_reserve(bud_buf *buf, size_t add)
{
	size_t need;
	size_t cap;
	char *data;

	need = buf->len + add + 1;
	if (need <= buf->cap) {
		return 0;
	}

	cap = buf->cap ? buf->cap : 128;
	while (cap < need) {
		cap *= 2;
	}

	data = (char *)realloc(buf->data, cap);
	if (!data) {
		return -1;
	}

	buf->data = data;
	buf->cap = cap;
	return 0;
}

static inline int bud_buf_append_n(bud_buf *buf, const char *text, size_t len)
{
	if (bud_buf_reserve(buf, len) != 0) {
		return -1;
	}

	memcpy(buf->data + buf->len, text, len);
	buf->len += len;
	buf->data[buf->len] = '\0';
	return 0;
}

static inline int bud_buf_append(bud_buf *buf, const char *text)
{
	if (!text) {
		return 0;
	}

	return bud_buf_append_n(buf, text, strlen(text));
}

static inline int bud_buf_append_char(bud_buf *buf, char ch)
{
	if (bud_buf_reserve(buf, 1) != 0) {
		return -1;
	}

	buf->data[buf->len++] = ch;
	buf->data[buf->len] = '\0';
	return 0;
}

static inline int bud_buf_append_escaped(bud_buf *buf, const char *text, int attr)
{
	const char *p;
	const char *start;

	if (!text) {
		return 0;
	}

	start = text;
	for (p = text; *p; p++) {
		const char *rep = NULL;
		size_t replen = 0;

		switch (*p) {
		case '&':
			rep = "&amp;";
			replen = 5;
			break;
		case '<':
			rep = "&lt;";
			replen = 4;
			break;
		case '>':
			rep = "&gt;";
			replen = 4;
			break;
		case '"':
			if (attr) {
				rep = "&quot;";
				replen = 6;
			}
			break;
		case '\'':
			if (attr) {
				rep = "&#39;";
				replen = 5;
			}
			break;
		default:
			break;
		}

		if (rep) {
			if (p > start) {
				if (bud_buf_append_n(buf, start, (size_t)(p - start)) != 0) {
					return -1;
				}
			}
			if (bud_buf_append_n(buf, rep, replen) != 0) {
				return -1;
			}
			start = p + 1;
		}
	}

	if (p > start) {
		if (bud_buf_append_n(buf, start, (size_t)(p - start)) != 0) {
			return -1;
		}
	}

	return 0;
}

static inline bud_node *bud_node_new(bud_node_kind kind)
{
	bud_node *node;
	int from_arena = 0;

	node = (bud_node *)bud_alloc_zero(
	        sizeof(*node), _Alignof(bud_node), &from_arena);
	if (!node) {
		return NULL;
	}

	node->kind = kind;
	if (from_arena) {
		node->flags |= BUD_NODE_ARENA;
	}
	return node;
}

static inline int bud_is_void_tag(const char *tag)
{
	static const char *const void_tags[] = { "area",  "base",  "br",
		                                 "col",   "embed", "hr",
		                                 "img",   "input", "link",
		                                 "meta",  "param", "source",
		                                 "track", "wbr",   NULL };
	int i;

	if (!tag) {
		return 0;
	}

	for (i = 0; void_tags[i]; i++) {
		if (strcmp(tag, void_tags[i]) == 0) {
			return 1;
		}
	}

	return 0;
}

static inline bud_attr *bud_attr_find(bud_node *node, const char *name)
{
	bud_attr *attr;

	for (attr = node->attrs; attr; attr = attr->next) {
		if (strcmp(attr->name, name) == 0) {
			return attr;
		}
	}

	return NULL;
}

static inline bud_listener *bud_listener_find(bud_node *node, const char *event)
{
	bud_listener *listener;

	for (listener = node->listeners; listener; listener = listener->next) {
		if (strcmp(listener->event, event) == 0) {
			return listener;
		}
	}

	return NULL;
}


void bud_node_call_mount(bud_node *node, bud_runtime *runtime);
void bud_node_call_update(bud_node *node, bud_runtime *runtime);
void bud_node_call_unmount(bud_node *node, bud_runtime *runtime);
void bud_node_set_text_copy(bud_node *node, const char *value);
extern bud_node *(*bud_ssr_expand_component_hook)(const bud_node *node);
bud_node *bud_ssr_expand_component(const bud_node *node);

#endif /* BUD_INTERNAL_H */
