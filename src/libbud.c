#include "bud_internal.h"

/* Platform host function pointers (set by WASM adapter, NULL on native) */
void (*bud_host_fetch_fn)(const char *, size_t, int) = NULL;
void (*bud_host_log_fn)(const char *, size_t) = NULL;
void (*bud_host_set_location_fn)(const char *, size_t) = NULL;


static void bud_attr_free(bud_attr *attr)
{
	bud_attr *next;

	while (attr) {
		next = attr->next;
		free(attr->name);
		free(attr->value);
		free(attr);
		attr = next;
	}
}

static void bud_listener_free(bud_listener *listener)
{
	bud_listener *next;

	while (listener) {
		next = listener->next;
		free(listener->event);
		if (listener->user_size > 0) {
			free(listener->user);
		}
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
		bud_free(child);
		child = next;
	}

	bud_attr_free(node->attrs);
	bud_listener_free(node->listeners);
	free(node->tag);
	free(node->text);
	free(node->key);
	free(node->comp_props);
	free(node);
}

bud_node *bud_fragment(void)
{
	return bud_node_new(BUD_NODE_FRAGMENT);
}

bud_node *bud_element(const char *tag)
{
	bud_node *node;

	if (!tag) {
		return NULL;
	}

	node = bud_node_new(BUD_NODE_ELEMENT);
	if (!node) {
		return NULL;
	}

	node->tag = bud_strdup(tag);
	if (!node->tag) {
		free(node);
		return NULL;
	}

	return node;
}

bud_node *bud_text(const char *text)
{
	bud_node *node;

	node = bud_node_new(BUD_NODE_TEXT);
	if (!node) {
		return NULL;
	}

	node->text = bud_strdup(text ? text : "");
	if (!node->text) {
		free(node);
		return NULL;
	}

	return node;
}

bud_node *bud_textf(const char *fmt, ...)
{
	char tmp[4096];
	va_list ap;

	if (!fmt) {
		return bud_text("");
	}
	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);

	return bud_text(tmp);
}

bud_node *bud_hidden_input(const char *name, const char *value)
{
	bud_node *input = bud_element("input");
	if (!input)
		return NULL;
	bud_set_attr(input, "type", "hidden");
	if (name)
		bud_set_attr(input, "name", name);
	if (value)
		bud_set_attr(input, "value", value);
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
	bud_node *btn = bud_element("button");
	if (!btn)
		return NULL;
	bud_set_attr(btn, "type", "submit");
	if (class_name && class_name[0])
		bud_set_attr(btn, "class", class_name);
	if (label && label[0])
		bud_append(btn, bud_text(label));
	return btn;
}

bud_node *bud_link(const char *href, const char *text, const char *class_name)
{
	bud_node *a = bud_element("a");
	if (!a)
		return NULL;
	if (href)
		bud_set_attr(a, "href", href);
	if (class_name && class_name[0])
		bud_set_attr(a, "class", class_name);
	if (text && text[0])
		bud_append(a, bud_text(text));
	return a;
}

bud_node *bud_raw(const char *html)
{
	bud_node *node;

	node = bud_node_new(BUD_NODE_RAW_HTML);
	if (!node) {
		return NULL;
	}

	node->text = bud_strdup(html ? html : "");
	if (!node->text) {
		free(node);
		return NULL;
	}

	return node;
}

void bud_raw_set_text(bud_node *node, const char *text)
{
	if (!node || node->kind != BUD_NODE_RAW_HTML)
		return;
	free(node->text);
	node->text = bud_strdup(text ? text : "");
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

int bud_set_attr(bud_node *node, const char *name, const char *value)
{
	bud_attr *attr;
	bud_attr *tail;
	char *name_copy;
	char *value_copy;

	if (!node || !name || !bud_is_valid_name(name)) {
		return -1;
	}

	if (node->kind == BUD_NODE_COMPONENT &&
	    strcmp(name, "data-bud-key") == 0)
	{
		value_copy = bud_strdup(value ? value : "");
		if (!value_copy) {
			return -1;
		}
		free(node->key);
		node->key = value_copy;
		return 0;
	}

	if (node->kind != BUD_NODE_ELEMENT) {
		return -1;
	}

	name_copy = bud_strdup(name);
	value_copy = bud_strdup(value ? value : "");
	if (!name_copy || !value_copy) {
		free(name_copy);
		free(value_copy);
		return -1;
	}

	attr = bud_attr_find(node, name);
	if (attr) {
		free(attr->value);
		attr->value = value_copy;
		free(name_copy);
		return 0;
	}

	attr = (bud_attr *)calloc(1, sizeof(*attr));
	if (!attr) {
		free(name_copy);
		free(value_copy);
		return -1;
	}

	attr->name = name_copy;
	attr->value = value_copy;
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

int bud_set_attr_fmt(bud_node *node, const char *name, const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	if (!fmt) {
		return bud_set_attr(node, name, "");
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

int bud_set_bool_attr(bud_node *node, const char *name)
{
	bud_attr *attr;
	bud_attr *tail;
	char *name_copy;

	if (!node || node->kind != BUD_NODE_ELEMENT || !name) {
		return -1;
	}

	attr = bud_attr_find(node, name);
	if (attr) {
		attr->is_boolean = 1;
		return 0;
	}

	name_copy = bud_strdup(name);
	if (!name_copy) {
		return -1;
	}

	attr = (bud_attr *)calloc(1, sizeof(*attr));
	if (!attr) {
		free(name_copy);
		return -1;
	}

	attr->name = name_copy;
	attr->is_boolean = 1;
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
	char *new_classes;
	size_t cur_len;
	size_t cls_len;

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
		new_classes = (char *)malloc(cur_len + 1 + cls_len + 1);
		if (!new_classes) {
			return -1;
		}
		memcpy(new_classes, cur, cur_len);
		new_classes[cur_len] = ' ';
		memcpy(new_classes + cur_len + 1, cls, cls_len + 1);
		bud_set_attr(node, "class", new_classes);
		free(new_classes);
	} else {
		bud_set_attr(node, "class", cls);
	}

	return 0;
}

int bud_remove_class(bud_node *node, const char *cls)
{
	const char *cur;
	char *p;
	char *new_classes;
	size_t cls_len;

	if (!node || node->kind != BUD_NODE_ELEMENT || !cls || !*cls) {
		return -1;
	}

	cur = bud_get_attr(node, "class");
	if (!cur || !bud_class_has(cur, cls)) {
		return 0;
	}

	cls_len = strlen(cls);
	new_classes = (char *)malloc(strlen(cur) + 1);
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
		bud_set_attr(node, "class", new_classes);
	} else {
		bud_set_attr(node, "class", "");
	}

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

int bud_bind_ctx(
        bud_node *node, const char *event, int bubbles,
        bud_event_handler_fn handler, const void *ctx, size_t ctx_size)
{
	bud_listener *listener;
	bud_listener *tail;
	char *event_copy;
	void *user_copy = NULL;

	if (!node || node->kind != BUD_NODE_ELEMENT || !event) {
		return -1;
	}

	if (ctx && ctx_size > 0) {
		user_copy = malloc(ctx_size);
		if (!user_copy) {
			return -1;
		}
		memcpy(user_copy, ctx, ctx_size);
	} else {
		user_copy = (void *)ctx;
	}

	listener = bud_listener_find(node, event);
	if (listener) {
		if (listener->user_size > 0) {
			free(listener->user);
		}
		listener->bubbles = bubbles ? 1 : 0;
		listener->handler = handler;
		listener->user = user_copy;
		listener->user_size = (ctx && ctx_size > 0) ? ctx_size : 0;
		return 0;
	}

	event_copy = bud_strdup(event);
	if (!event_copy) {
		if (ctx && ctx_size > 0) {
			free(user_copy);
		}
		return -1;
	}

	listener = (bud_listener *)calloc(1, sizeof(*listener));
	if (!listener) {
		free(event_copy);
		if (ctx && ctx_size > 0) {
			free(user_copy);
		}
		return -1;
	}

	listener->event = event_copy;
	listener->bubbles = bubbles ? 1 : 0;
	listener->handler = handler;
	listener->user = user_copy;
	listener->user_size = (ctx && ctx_size > 0) ? ctx_size : 0;
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
		node->comp_props = malloc(props_size);
		if (!node->comp_props) {
			free(node);
			return NULL;
		}
		memcpy(node->comp_props, props, props_size);
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
