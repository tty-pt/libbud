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

typedef struct bud_attr bud_attr;
typedef struct bud_listener bud_listener;

struct bud_attr {
	char *name;
	char *value;
	int is_boolean;
	bud_attr *next;
};

struct bud_listener {
	char *event;
	int bubbles;
	bud_event_handler_fn handler;
	void *user;
	size_t user_size;
	bud_listener *next;
};

struct bud_node {
	bud_node_kind kind;
	unsigned int id;
	char *tag;
	char *text;
	char *key;
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
	int mounted;
	/* Set on a root that came out of bud_vdom_diff: its ids are now the ids
	 * the DOM carries (matched nodes inherit, inserted nodes got fresh ones),
	 * so bud_prepare_render must not renumber it DFS again. */
	int ids_frozen;
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

	node = (bud_node *)calloc(1, sizeof(*node));
	if (!node) {
		return NULL;
	}

	node->kind = kind;
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
extern bud_node *(*bud_ssr_expand_component_hook)(const bud_node *node);
bud_node *bud_ssr_expand_component(const bud_node *node);

#endif /* BUD_INTERNAL_H */
