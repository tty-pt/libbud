#include "bud_internal.h"

/* Platform host function pointers (set by WASM adapter, NULL on native) */
void (*bud_host_fetch_fn)(const char *, size_t, int) = NULL;
void (*bud_host_log_fn)(const char *, size_t) = NULL;
void (*bud_host_set_location_fn)(const char *, size_t) = NULL;

/* ── Per-render bump arena (bud_arena) ───────────────────────────────── */

BUD_TLS bud_arena *bud_active_arena = NULL;

static bud_arena_slab *bud_arena_slab_new(size_t cap)
{
	bud_arena_slab *s;
	if (cap > SIZE_MAX - sizeof(*s))
		return NULL;
	s = (bud_arena_slab *)malloc(sizeof(*s) + cap);
	if (!s)
		return NULL;
	s->next = NULL;
	s->used = 0;
	s->cap = cap;
	return s;
}

bud_arena *bud_arena_new(size_t initial_cap)
{
	bud_arena *arena;
	bud_arena_slab *slab;

	if (initial_cap == 0)
		initial_cap = BUD_ARENA_DEFAULT_CAP;

	arena = (bud_arena *)malloc(sizeof(*arena));
	if (!arena)
		return NULL;

	slab = bud_arena_slab_new(initial_cap);
	if (!slab) {
		free(arena);
		return NULL;
	}

	arena->head = slab;
	arena->cur = slab;
	arena->initial_cap = initial_cap;
	arena->total_used = 0;
	arena->slab_count = 1;
	return arena;
}

void *bud_arena_alloc_slow(bud_arena *arena, size_t size, size_t align)
{
	bud_arena_slab *cur;
	bud_arena_slab *next;
	bud_arena_slab *slab;
	size_t next_cap;

	(void)align;
	if (!arena)
		return NULL;

	cur = arena->cur;

	/* Reuse a warm pre-allocated slab after bud_arena_reset() when it fits. */
	next = cur ? cur->next : NULL;
	if (next && next->cap >= size) {
		next->used = size;
		arena->cur = next;
		arena->total_used += size;
		return next->data;
	}

	next_cap = cur ? cur->cap : arena->initial_cap;
	if (next_cap < BUD_ARENA_MAX_SLAB_CAP) {
		next_cap *= 2;
		if (next_cap > BUD_ARENA_MAX_SLAB_CAP)
			next_cap = BUD_ARENA_MAX_SLAB_CAP;
	}
	if (next_cap < size)
		next_cap = size;

	slab = bud_arena_slab_new(next_cap);
	if (!slab)
		return NULL;

	if (cur) {
		slab->next = cur->next;
		cur->next = slab;
	} else {
		arena->head = slab;
	}
	arena->cur = slab;
	arena->slab_count++;
	slab->used = size;
	arena->total_used += size;
	return slab->data;
}

bud_arena *bud_arena_push(bud_arena *arena)
{
	bud_arena *prev = bud_active_arena;
	bud_active_arena = arena;
	return prev;
}

void bud_arena_pop(bud_arena *prev)
{
	bud_active_arena = prev;
}

bud_arena *bud_arena_current(void)
{
	return bud_active_arena;
}

void bud_arena_reset(bud_arena *arena)
{
	bud_arena_slab *s;
	bud_arena_slab *prev = NULL;
	size_t kept_bytes = 0;
	size_t kept_slabs = 0;

	if (!arena)
		return;

	s = arena->head;
	while (s) {
		bud_arena_slab *next = s->next;
		if (kept_slabs == 0 ||
		    (s->cap <= BUD_ARENA_MAX_SLAB_CAP &&
		     kept_bytes + s->cap <= BUD_ARENA_MAX_RETAIN_BYTES))
		{
			s->used = 0;
			kept_bytes += s->cap;
			kept_slabs++;
			prev = s;
		} else {
			if (prev)
				prev->next = next;
			else
				arena->head = next;
			free(s);
		}
		s = next;
	}
	arena->cur = arena->head;
	arena->total_used = 0;
	arena->slab_count = kept_slabs;
}

void bud_arena_free(bud_arena *arena)
{
	bud_arena_slab *s;

	if (!arena)
		return;

	if (bud_active_arena == arena)
		bud_active_arena = NULL;

	s = arena->head;
	while (s) {
		bud_arena_slab *next = s->next;
		free(s);
		s = next;
	}
	free(arena);
}

size_t bud_arena_bytes_used(const bud_arena *arena)
{
	return arena ? arena->total_used : 0;
}

/* ── Node ownership & lifecycle ──────────────────────────────────────── */

static void bud_attr_free(bud_attr *attr)
{
	bud_attr *next;

	while (attr) {
		next = attr->next;
		if (attr->flags & BUD_ATTR_NAME_OWNED)
			free((void *)attr->name);
		if (attr->flags & BUD_ATTR_VAL_OWNED)
			free((void *)attr->value);
		if (!(attr->flags & BUD_ATTR_ARENA))
			free(attr);
		attr = next;
	}
}

static void bud_listener_free(bud_listener *listener)
{
	bud_listener *next;

	while (listener) {
		next = listener->next;
		if (listener->flags & BUD_LISTENER_EVENT_OWNED)
			free((void *)listener->event);
		if (listener->flags & BUD_LISTENER_USER_OWNED)
			free(listener->user);
		if (!(listener->flags & BUD_LISTENER_ARENA))
			free(listener);
		listener = next;
	}
}

void bud_node_call_mount(bud_node *node, bud_runtime *runtime)
{
	bud_node *child;

	if (!node) {
		return;
	}

	if (node->mounted) {
		return;
	}

	node->mounted = 1;
	if (node->on_mount) {
		node->on_mount(node->lifecycle_user, runtime, node);
	}

	for (child = node->first_child; child; child = child->next_sibling) {
		bud_node_call_mount(child, runtime);
	}
}

void bud_node_call_update(bud_node *node, bud_runtime *runtime)
{
	bud_node *child;

	if (!node || !node->mounted) {
		return;
	}

	if (node->on_update) {
		node->on_update(node->lifecycle_user, runtime, node);
	}

	for (child = node->first_child; child; child = child->next_sibling) {
		bud_node_call_update(child, runtime);
	}
}

void bud_node_call_unmount(bud_node *node, bud_runtime *runtime)
{
	bud_node *child;
	bud_node *next;

	if (!node || !node->mounted) {
		return;
	}

	child = node->first_child;
	while (child) {
		next = child->next_sibling;
		bud_node_call_unmount(child, runtime);
		child = next;
	}

	if (node->on_unmount) {
		node->on_unmount(node->lifecycle_user, runtime, node);
	}
	node->mounted = 0;
}

static void bud_node_free_chain(bud_node *node)
{
	bud_node *child;
	bud_node *next;

	if (!node) {
		return;
	}

	bud_node_call_unmount(node, NULL);
	child = node->first_child;
	while (child) {
		next = child->next_sibling;
		bud_node_free_chain(child);
		child = next;
	}

	bud_attr_free(node->attrs);
	bud_listener_free(node->listeners);
	if (node->flags & BUD_NODE_TAG_OWNED)
		free((void *)node->tag);
	if (node->flags & BUD_NODE_TEXT_OWNED)
		free((void *)node->text);
	if (node->flags & BUD_NODE_KEY_OWNED)
		free((void *)node->key);
	if (node->flags & BUD_NODE_PROPS_OWNED)
		free(node->comp_props);
	if (!(node->flags & BUD_NODE_ARENA))
		free(node);
}

bud_node *bud_fragment(void)
{
	return bud_node_new(BUD_NODE_FRAGMENT);
}

static bud_node *bud_element_impl(const char *tag, int is_lit)
{
	bud_node *node;

	if (!tag) {
		return NULL;
	}

	node = bud_node_new(BUD_NODE_ELEMENT);
	if (!node) {
		return NULL;
	}

	if (is_lit) {
		node->tag = tag;
	} else {
		int owned = 0;
		node->tag = bud_alloc_str(tag, &owned);
		if (!node->tag) {
			if (!(node->flags & BUD_NODE_ARENA))
				free(node);
			return NULL;
		}
		if (owned)
			node->flags |= BUD_NODE_TAG_OWNED;
	}

	return node;
}

bud_node *bud_element(const char *tag)
{
	return bud_element_impl(tag, 0);
}

bud_node *bud_element_lit(const char *tag_lit)
{
	return bud_element_impl(tag_lit, 1);
}

static bud_node *bud_text_impl(const char *text, int is_lit)
{
	bud_node *node;
	const char *src = text ? text : "";

	node = bud_node_new(BUD_NODE_TEXT);
	if (!node) {
		return NULL;
	}

	if (is_lit) {
		node->text = src;
	} else {
		int owned = 0;
		node->text = bud_alloc_str(src, &owned);
		if (!node->text) {
			if (!(node->flags & BUD_NODE_ARENA))
				free(node);
			return NULL;
		}
		if (owned)
			node->flags |= BUD_NODE_TEXT_OWNED;
	}

	return node;
}

bud_node *bud_text(const char *text)
{
	return bud_text_impl(text, 0);
}

bud_node *bud_text_lit(const char *text_lit)
{
	return bud_text_impl(text_lit, 1);
}

bud_node *bud_textf(const char *fmt, ...)
{
	char tmp[4096];
	va_list ap;

	if (!fmt) {
		return bud_text_lit("");
	}
	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);

	return bud_text(tmp);
}

bud_node *bud_hidden_input(const char *name, const char *value)
{
	bud_node *input = bud_element_lit("input");
	if (!input)
		return NULL;
	bud_set_attr_lit(input, "type", "hidden");
	if (name)
		bud_set_attr_name_lit(input, "name", name);
	if (value)
		bud_set_attr_name_lit(input, "value", value);
	return input;
}

bud_node *bud_hidden_input_int(const char *name, int value)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%d", value);
	return bud_hidden_input(name, buf);
}

bud_node *bud_submit_btn(const char *label, const char *class_name)
{
	bud_node *btn = bud_element_lit("button");
	if (!btn)
		return NULL;
	bud_set_attr_lit(btn, "type", "submit");
	if (class_name && class_name[0])
		bud_set_attr_name_lit(btn, "class", class_name);
	if (label && label[0])
		bud_append(btn, bud_text(label));
	return btn;
}

bud_node *bud_link(const char *href, const char *text, const char *class_name)
{
	bud_node *a = bud_element_lit("a");
	if (!a)
		return NULL;
	if (href)
		bud_set_attr_name_lit(a, "href", href);
	if (class_name && class_name[0])
		bud_set_attr_name_lit(a, "class", class_name);
	if (text && text[0])
		bud_append(a, bud_text(text));
	return a;
}

bud_node *bud_raw(const char *html)
{
	bud_node *node;
	int owned = 0;

	node = bud_node_new(BUD_NODE_RAW_HTML);
	if (!node) {
		return NULL;
	}

	node->text = bud_alloc_str(html ? html : "", &owned);
	if (!node->text) {
		if (!(node->flags & BUD_NODE_ARENA))
			free(node);
		return NULL;
	}
	if (owned)
		node->flags |= BUD_NODE_TEXT_OWNED;

	return node;
}

void bud_node_set_text_copy(bud_node *node, const char *value)
{
	int owned = 0;
	const char *copy;

	if (!node)
		return;
	copy = bud_alloc_str(value ? value : "", &owned);
	if (!copy)
		return;
	if (node->flags & BUD_NODE_TEXT_OWNED)
		free((void *)node->text);
	node->text = copy;
	if (owned)
		node->flags |= BUD_NODE_TEXT_OWNED;
	else
		node->flags &= (uint8_t)~BUD_NODE_TEXT_OWNED;
}

void bud_raw_set_text(bud_node *node, const char *text)
{
	if (!node || node->kind != BUD_NODE_RAW_HTML)
		return;
	bud_node_set_text_copy(node, text);
}

bud_node *
bud_component_render(const bud_component *component, const void *props)
{
	if (!component || !component->render) {
		return NULL;
	}

	return component->render(component->ctx, props);
}

static int bud_is_valid_name(const char *name)
{
	const char *p;
	if (!name || !name[0])
		return 0;
	if (!((name[0] >= 'a' && name[0] <= 'z') ||
	      (name[0] >= 'A' && name[0] <= 'Z') || name[0] == '_'))
		return 0;
	for (p = name + 1; *p; p++) {
		char c = *p;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ':')
			continue;
		return 0;
	}
	return 1;
}

static int bud_set_attr_impl(
        bud_node *node, const char *name, int name_lit, const char *value,
        int val_lit)
{
	bud_attr *attr;
	bud_attr *tail;
	const char *name_store;
	const char *value_store;
	int name_owned = 0;
	int val_owned = 0;
	int from_arena = 0;
	const char *val_src = value ? value : "";

	if (!node || !name || !bud_is_valid_name(name)) {
		return -1;
	}

	if (node->kind == BUD_NODE_COMPONENT &&
	    strcmp(name, "data-bud-key") == 0)
	{
		if (val_lit) {
			value_store = val_src;
		} else {
			value_store = bud_alloc_str(val_src, &val_owned);
			if (!value_store) {
				return -1;
			}
		}
		if (node->flags & BUD_NODE_KEY_OWNED)
			free((void *)node->key);
		node->key = value_store;
		if (val_owned)
			node->flags |= BUD_NODE_KEY_OWNED;
		else
			node->flags &= (uint8_t)~BUD_NODE_KEY_OWNED;
		return 0;
	}

	if (node->kind != BUD_NODE_ELEMENT) {
		return -1;
	}

	if (val_lit) {
		value_store = val_src;
	} else {
		value_store = bud_alloc_str(val_src, &val_owned);
		if (!value_store) {
			return -1;
		}
	}

	attr = bud_attr_find(node, name);
	if (attr) {
		if (attr->flags & BUD_ATTR_VAL_OWNED)
			free((void *)attr->value);
		attr->value = value_store;
		if (val_owned)
			attr->flags |= BUD_ATTR_VAL_OWNED;
		else
			attr->flags &= (uint8_t)~BUD_ATTR_VAL_OWNED;
		return 0;
	}

	if (name_lit) {
		name_store = name;
	} else {
		name_store = bud_alloc_str(name, &name_owned);
		if (!name_store) {
			if (val_owned)
				free((void *)value_store);
			return -1;
		}
	}

	attr = (bud_attr *)bud_alloc_zero(
	        sizeof(*attr), _Alignof(bud_attr), &from_arena);
	if (!attr) {
		if (name_owned)
			free((void *)name_store);
		if (val_owned)
			free((void *)value_store);
		return -1;
	}

	attr->name = name_store;
	attr->value = value_store;
	if (from_arena)
		attr->flags |= BUD_ATTR_ARENA;
	if (name_owned)
		attr->flags |= BUD_ATTR_NAME_OWNED;
	if (val_owned)
		attr->flags |= BUD_ATTR_VAL_OWNED;

	if (!node->attrs) {
		node->attrs = attr;
		return 0;
	}

	tail = node->attrs;
	while (tail->next) {
		tail = tail->next;
	}

	tail->next = attr;
	return 0;
}

int bud_set_attr(bud_node *node, const char *name, const char *value)
{
	return bud_set_attr_impl(node, name, 0, value, 0);
}

int bud_set_attr_lit(
        bud_node *node, const char *name_lit, const char *value_lit)
{
	return bud_set_attr_impl(node, name_lit, 1, value_lit, 1);
}

int bud_set_attr_name_lit(
        bud_node *node, const char *name_lit, const char *value)
{
	return bud_set_attr_impl(node, name_lit, 1, value, 0);
}

int bud_set_attr_fmt(bud_node *node, const char *name, const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	if (!fmt) {
		return bud_set_attr_impl(node, name, 0, "", 1);
	}
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	return bud_set_attr(node, name, buf);
}

bud_arg bud_attr_fmt(const char *name, const char *fmt, ...)
{
	char tmp[512];
	va_list ap;
	char *heap;
	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	heap = bud_strdup(tmp);
	if (!heap)
		heap = bud_strdup("");
	return (bud_arg){ .type = BUD_ARG_ATTR_FMT,
		          .data.attr = { name, heap ? heap : "" } };
}

static int bud_set_bool_attr_impl(bud_node *node, const char *name, int name_lit)
{
	bud_attr *attr;
	bud_attr *tail;
	const char *name_store;
	int name_owned = 0;
	int from_arena = 0;

	if (!node || node->kind != BUD_NODE_ELEMENT || !name) {
		return -1;
	}

	attr = bud_attr_find(node, name);
	if (attr) {
		attr->is_boolean = 1;
		return 0;
	}

	if (name_lit) {
		name_store = name;
	} else {
		name_store = bud_alloc_str(name, &name_owned);
		if (!name_store) {
			return -1;
		}
	}

	attr = (bud_attr *)bud_alloc_zero(
	        sizeof(*attr), _Alignof(bud_attr), &from_arena);
	if (!attr) {
		if (name_owned)
			free((void *)name_store);
		return -1;
	}

	attr->name = name_store;
	attr->is_boolean = 1;
	if (from_arena)
		attr->flags |= BUD_ATTR_ARENA;
	if (name_owned)
		attr->flags |= BUD_ATTR_NAME_OWNED;

	if (!node->attrs) {
		node->attrs = attr;
		return 0;
	}

	tail = node->attrs;
	while (tail->next) {
		tail = tail->next;
	}

	tail->next = attr;
	return 0;
}

int bud_set_bool_attr(bud_node *node, const char *name)
{
	return bud_set_bool_attr_impl(node, name, 0);
}

int bud_set_bool_attr_lit(bud_node *node, const char *name_lit)
{
	return bud_set_bool_attr_impl(node, name_lit, 1);
}

const char *bud_get_attr(const bud_node *node, const char *name)
{
	bud_attr *attr;

	if (!node || !name) {
		return NULL;
	}

	if (node->kind == BUD_NODE_COMPONENT &&
	    strcmp(name, "data-bud-key") == 0)
	{
		return node->key;
	}

	if (node->kind != BUD_NODE_ELEMENT) {
		return NULL;
	}

	attr = bud_attr_find((bud_node *)node, name);
	if (!attr) {
		return NULL;
	}

	return attr->value;
}

static int bud_class_has(const char *classes, const char *cls)
{
	const char *p;
	size_t len;

	if (!classes || !cls || !*cls) {
		return 0;
	}

	len = strlen(cls);
	p = classes;
	while (*p) {
		while (*p == ' ') {
			p++;
		}
		if (!*p) {
			break;
		}
		if (strncmp(p, cls, len) == 0 &&
		    (p[len] == ' ' || p[len] == '\0'))
		{
			return 1;
		}
		while (*p && *p != ' ') {
			p++;
		}
	}

	return 0;
}

int bud_add_class(bud_node *node, const char *cls)
{
	const char *cur;
	char stack_buf[256];
	char *new_classes;
	size_t cur_len;
	size_t cls_len;
	size_t need;

	if (!node || node->kind != BUD_NODE_ELEMENT || !cls || !*cls) {
		return -1;
	}

	cur = bud_get_attr(node, "class");
	if (bud_class_has(cur, cls)) {
		return 0;
	}

	cls_len = strlen(cls);
	if (cur) {
		cur_len = strlen(cur);
		need = cur_len + 1 + cls_len + 1;
		new_classes = need <= sizeof(stack_buf)
		                      ? stack_buf
		                      : (char *)malloc(need);
		if (!new_classes) {
			return -1;
		}
		memcpy(new_classes, cur, cur_len);
		new_classes[cur_len] = ' ';
		memcpy(new_classes + cur_len + 1, cls, cls_len + 1);
		bud_set_attr_name_lit(node, "class", new_classes);
		if (new_classes != stack_buf)
			free(new_classes);
	} else {
		bud_set_attr_name_lit(node, "class", cls);
	}

	return 0;
}

int bud_remove_class(bud_node *node, const char *cls)
{
	const char *cur;
	char *p;
	char stack_buf[256];
	char *new_classes;
	size_t cls_len;
	size_t need;

	if (!node || node->kind != BUD_NODE_ELEMENT || !cls || !*cls) {
		return -1;
	}

	cur = bud_get_attr(node, "class");
	if (!cur || !bud_class_has(cur, cls)) {
		return 0;
	}

	cls_len = strlen(cls);
	need = strlen(cur) + 1;
	new_classes = need <= sizeof(stack_buf) ? stack_buf : (char *)malloc(need);
	if (!new_classes) {
		return -1;
	}

	p = new_classes;
	while (*cur) {
		while (*cur == ' ') {
			cur++;
		}
		if (!*cur) {
			break;
		}
		if (strncmp(cur, cls, cls_len) == 0 &&
		    (cur[cls_len] == ' ' || cur[cls_len] == '\0'))
		{
			cur += cls_len;
			continue;
		}
		while (*cur && *cur != ' ') {
			*p++ = *cur++;
		}
		if (*cur == ' ') {
			*p++ = ' ';
		}
	}
	if (p > new_classes && *(p - 1) == ' ') {
		*(p - 1) = '\0';
	} else {
		*p = '\0';
	}

	if (*new_classes) {
		bud_set_attr_name_lit(node, "class", new_classes);
	} else {
		bud_set_attr_lit(node, "class", "");
	}

	if (new_classes != stack_buf)
		free(new_classes);
	return 0;
}

int bud_toggle_class(bud_node *node, const char *cls)
{
	const char *cur;

	if (!node || node->kind != BUD_NODE_ELEMENT || !cls || !*cls) {
		return -1;
	}

	cur = bud_get_attr(node, "class");
	if (bud_class_has(cur, cls)) {
		return bud_remove_class(node, cls);
	}

	return bud_add_class(node, cls);
}

int bud_detach(bud_node *node)
{
	bud_node *child;
	bud_node *prev;

	if (!node || !node->parent) {
		return -1;
	}

	prev = NULL;
	child = node->parent->first_child;
	while (child) {
		if (child == node) {
			if (prev) {
				prev->next_sibling = node->next_sibling;
			} else {
				node->parent->first_child = node->next_sibling;
			}
			if (node->parent->last_child == node) {
				node->parent->last_child = prev;
			}
			node->parent = NULL;
			node->next_sibling = NULL;
			return 0;
		}
		prev = child;
		child = child->next_sibling;
	}

	return -1;
}

int bud_on(bud_node *node, const char *event, int bubbles)
{
	return bud_bind(node, event, bubbles, NULL);
}

static int bud_bind_ctx_impl(
        bud_node *node, const char *event, int event_lit, int bubbles,
        bud_event_handler_fn handler, const void *ctx, size_t ctx_size)
{
	bud_listener *listener;
	bud_listener *tail;
	const char *event_store;
	void *user_copy = NULL;
	int event_owned = 0;
	int user_owned = 0;
	int from_arena = 0;

	if (!node || node->kind != BUD_NODE_ELEMENT || !event) {
		return -1;
	}

	if (ctx && ctx_size > 0) {
		user_copy = bud_alloc_bytes(ctx, ctx_size, &user_owned);
		if (!user_copy) {
			return -1;
		}
	} else {
		user_copy = (void *)ctx;
	}

	listener = bud_listener_find(node, event);
	if (listener) {
		if (listener->flags & BUD_LISTENER_USER_OWNED) {
			free(listener->user);
		}
		listener->bubbles = bubbles ? 1 : 0;
		listener->handler = handler;
		listener->user = user_copy;
		listener->user_size = (ctx && ctx_size > 0) ? ctx_size : 0;
		if (user_owned)
			listener->flags |= BUD_LISTENER_USER_OWNED;
		else
			listener->flags &= (uint8_t)~BUD_LISTENER_USER_OWNED;
		return 0;
	}

	if (event_lit) {
		event_store = event;
	} else {
		event_store = bud_alloc_str(event, &event_owned);
		if (!event_store) {
			if (user_owned)
				free(user_copy);
			return -1;
		}
	}

	listener = (bud_listener *)bud_alloc_zero(
	        sizeof(*listener), _Alignof(bud_listener), &from_arena);
	if (!listener) {
		if (event_owned)
			free((void *)event_store);
		if (user_owned)
			free(user_copy);
		return -1;
	}

	listener->event = event_store;
	listener->bubbles = bubbles ? 1 : 0;
	listener->handler = handler;
	listener->user = user_copy;
	listener->user_size = (ctx && ctx_size > 0) ? ctx_size : 0;
	if (from_arena)
		listener->flags |= BUD_LISTENER_ARENA;
	if (event_owned)
		listener->flags |= BUD_LISTENER_EVENT_OWNED;
	if (user_owned)
		listener->flags |= BUD_LISTENER_USER_OWNED;

	if (!node->listeners) {
		node->listeners = listener;
		return 0;
	}

	tail = node->listeners;
	while (tail->next) {
		tail = tail->next;
	}

	tail->next = listener;
	return 0;
}

int bud_bind_ctx(
        bud_node *node, const char *event, int bubbles,
        bud_event_handler_fn handler, const void *ctx, size_t ctx_size)
{
	return bud_bind_ctx_impl(node, event, 0, bubbles, handler, ctx, ctx_size);
}

int bud_bind_ctx_lit(
        bud_node *node, const char *event_lit, int bubbles,
        bud_event_handler_fn handler, const void *ctx, size_t ctx_size)
{
	return bud_bind_ctx_impl(
	        node, event_lit, 1, bubbles, handler, ctx, ctx_size);
}

int bud_bind(
        bud_node *node, const char *event, int bubbles,
        bud_event_handler_fn handler)
{
	return bud_bind_ctx(node, event, bubbles, handler, NULL, 0);
}

bud_node *bud_component_vnode(
        bud_component_fn fn, void *ctx, const void *props, size_t props_size)
{
	bud_node *node;

	if (!fn) {
		return NULL;
	}

	node = bud_node_new(BUD_NODE_COMPONENT);
	if (!node) {
		return NULL;
	}

	node->comp_fn = fn;
	node->comp_ctx = ctx;
	if (props && props_size > 0) {
		int props_owned = 0;
		node->comp_props = bud_alloc_bytes(props, props_size, &props_owned);
		if (!node->comp_props) {
			if (!(node->flags & BUD_NODE_ARENA))
				free(node);
			return NULL;
		}
		if (props_owned)
			node->flags |= BUD_NODE_PROPS_OWNED;
		node->comp_props_size = props_size;
	}
	return node;
}

bud_node *(*bud_ssr_expand_component_hook)(const bud_node *node) = NULL;

bud_node *bud_ssr_expand_component(const bud_node *node)
{
	if (!node || node->kind != BUD_NODE_COMPONENT || !node->comp_fn) {
		return NULL;
	}
	if (bud_ssr_expand_component_hook) {
		return bud_ssr_expand_component_hook(node);
	}
	return node->comp_fn(node->comp_ctx, node->comp_props);
}

int bud_set_lifecycle(
        bud_node *node, bud_lifecycle_fn on_mount, bud_lifecycle_fn on_update,
        bud_lifecycle_fn on_unmount, void *user)
{
	if (!node) {
		return -1;
	}

	node->on_mount = on_mount;
	node->on_update = on_update;
	node->on_unmount = on_unmount;
	node->lifecycle_user = user;
	return 0;
}

int bud_append(bud_node *parent, bud_node *child)
{
	if (!parent || !child) {
		return -1;
	}

	if (parent->kind == BUD_NODE_TEXT || parent->kind == BUD_NODE_RAW_HTML)
	{
		return -1;
	}

	child->parent = parent;
	child->next_sibling = NULL;
	if (!parent->first_child) {
		parent->first_child = child;
		parent->last_child = child;
		return 0;
	}

	parent->last_child->next_sibling = child;
	parent->last_child = child;
	return 0;
}

bud_node_kind bud_node_kind_of(const bud_node *node)
{
	if (!node) {
		return BUD_NODE_FRAGMENT;
	}

	return node->kind;
}

const char *bud_node_tag(const bud_node *node)
{
	if (!node) {
		return NULL;
	}

	return node->tag;
}

const char *bud_node_text(const bud_node *node)
{
	if (!node) {
		return NULL;
	}

	return node->text;
}

unsigned int bud_node_id(const bud_node *node)
{
	if (!node) {
		return 0;
	}

	return node->id;
}

static size_t bud_count_children(const bud_node *node)
{
	const bud_node *child;
	size_t count;

	count = 0;
	if (!node) {
		return 0;
	}

	for (child = node->first_child; child; child = child->next_sibling) {
		count++;
	}

	return count;
}

size_t bud_node_child_count(const bud_node *node)
{
	return bud_count_children(node);
}

const bud_node *bud_node_child(const bud_node *node, size_t index)
{
	const bud_node *child;
	size_t i;

	if (!node) {
		return NULL;
	}

	child = node->first_child;
	for (i = 0; child; i++) {
		if (i == index) {
			return child;
		}
		child = child->next_sibling;
	}

	return NULL;
}

bud_node *bud_node_parent(const bud_node *node)
{
	return node ? node->parent : NULL;
}

static const bud_attr *bud_attr_at(const bud_node *node, size_t index)
{
	const bud_attr *attr;
	size_t i;

	if (!node) {
		return NULL;
	}

	attr = node->attrs;
	for (i = 0; attr; i++) {
		if (i == index) {
			return attr;
		}
		attr = attr->next;
	}

	return NULL;
}

size_t bud_node_attr_count(const bud_node *node)
{
	size_t count;
	const bud_attr *attr;

	count = 0;
	if (!node) {
		return 0;
	}

	for (attr = node->attrs; attr; attr = attr->next) {
		count++;
	}

	return count;
}

const char *bud_node_attr_name(const bud_node *node, size_t index)
{
	const bud_attr *attr;

	attr = bud_attr_at(node, index);
	if (!attr) {
		return NULL;
	}

	return attr->name;
}

const char *bud_node_attr_value(const bud_node *node, size_t index)
{
	const bud_attr *attr;

	attr = bud_attr_at(node, index);
	if (!attr) {
		return NULL;
	}

	return attr->value;
}

static const bud_listener *bud_listener_at(const bud_node *node, size_t index)
{
	const bud_listener *listener;
	size_t i;

	if (!node) {
		return NULL;
	}

	listener = node->listeners;
	for (i = 0; listener; i++) {
		if (i == index) {
			return listener;
		}
		listener = listener->next;
	}

	return NULL;
}

size_t bud_node_listener_count(const bud_node *node)
{
	size_t count;
	const bud_listener *listener;

	count = 0;
	if (!node) {
		return 0;
	}

	for (listener = node->listeners; listener; listener = listener->next) {
		count++;
	}

	return count;
}

const char *bud_node_listener_event(const bud_node *node, size_t index)
{
	const bud_listener *listener;

	listener = bud_listener_at(node, index);
	if (!listener) {
		return NULL;
	}

	return listener->event;
}

int bud_node_listener_bubbles(const bud_node *node, size_t index)
{
	const bud_listener *listener;

	listener = bud_listener_at(node, index);
	if (!listener) {
		return -1;
	}

	return listener->bubbles;
}

void bud_free(bud_node *node)
{
	bud_node_free_chain(node);
}

void bud_free_string(char *value)
{
	free(value);
}

void bud_node_set_src(bud_node *node, const char *file, int line)
{
	if (!node)
		return;
#ifdef BUD_DEBUG
	node->src_file = file;
	node->src_line = line;
#else
	(void)file;
	(void)line;
#endif
}

static char bud_src_buf[256];

const char *bud_node_get_src(const bud_node *node)
{
	if (!node)
		return "(null)";
#ifdef BUD_DEBUG
	if (node->src_file) {
		snprintf(
		        bud_src_buf, sizeof(bud_src_buf), "%s:%d",
		        node->src_file, node->src_line);
		return bud_src_buf;
	}
#endif
	snprintf(
	        bud_src_buf, sizeof(bud_src_buf), "node=%d kind=%u tag=%s",
	        bud_node_id(node), (unsigned)bud_node_kind_of(node),
	        bud_node_tag(node) ? bud_node_tag(node) : "(null)");
	return bud_src_buf;
}
