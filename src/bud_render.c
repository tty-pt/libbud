#include "bud_internal.h"

static unsigned int bud_assign_ids(bud_node *node, unsigned int next_id)
{
	bud_node *child;

	if (!node) {
		return next_id;
	}

	node->id = next_id++;
	for (child = node->first_child; child; child = child->next_sibling) {
		next_id = bud_assign_ids(child, next_id);
	}

	return next_id;
}

static void bud_prepare_render(bud_node *root)
{
	if (root && root->ids_frozen) {
		return;
	}
	bud_assign_ids(root, 0);
}

static void bud_expand_children(bud_node *parent);

static bud_node *bud_expand_node_once(bud_node *node)
{
	const char *key = bud_get_key(node);
	bud_node *exp = bud_ssr_expand_component(node);

	if (exp && key && key[0] && exp->kind == BUD_NODE_ELEMENT &&
	    !bud_get_key(exp))
	{
		bud_set_key(exp, key);
	}
	return exp;
}

bud_node *bud_expand_tree(bud_node *root)
{
	int depth = 0;

	while (root && root->kind == BUD_NODE_COMPONENT && depth < 64) {
		bud_node *exp = bud_expand_node_once(root);
		root->parent = NULL;
		root->next_sibling = NULL;
		bud_free(root);
		root = exp;
		depth++;
	}
	if (!root) {
		return NULL;
	}
	bud_expand_children(root);
	return root;
}

static void bud_expand_children(bud_node *parent)
{
	bud_node *prev = NULL;
	bud_node *child;

	if (!parent) {
		return;
	}

	child = parent->first_child;
	while (child) {
		bud_node *next = child->next_sibling;
		if (child->kind == BUD_NODE_COMPONENT) {
			bud_node *exp = bud_expand_tree(child);
			if (exp) {
				exp->parent = parent;
				exp->next_sibling = next;
				if (prev) {
					prev->next_sibling = exp;
				} else {
					parent->first_child = exp;
				}
				if (!next) {
					parent->last_child = exp;
				}
				prev = exp;
			} else {
				if (prev) {
					prev->next_sibling = next;
				} else {
					parent->first_child = next;
				}
				if (!next) {
					parent->last_child = prev;
				}
			}
		} else {
			bud_expand_children(child);
			prev = child;
		}
		child = next;
	}
}

static int bud_append_attr(bud_buf *buf, const char *name, const char *value)
{
	if (bud_buf_append_char(buf, ' ') != 0) {
		return -1;
	}
	if (bud_buf_append(buf, name) != 0) {
		return -1;
	}
	if (bud_buf_append(buf, "=\"") != 0) {
		return -1;
	}
	if (bud_buf_append_escaped(buf, value, 1) != 0) {
		return -1;
	}
	if (bud_buf_append_char(buf, '"') != 0) {
		return -1;
	}

	return 0;
}

static int bud_listeners_to_attr(const bud_node *node, bud_buf *buf)
{
	const bud_listener *listener;
	char mode_buf[8];
	int len;
	int first;

	first = 1;
	for (listener = node->listeners; listener; listener = listener->next) {
		if (!first) {
			if (bud_buf_append_char(buf, ',') != 0) {
				return -1;
			}
		}
		if (bud_buf_append(buf, listener->event) != 0) {
			return -1;
		}
		if (bud_buf_append_char(buf, ':') != 0) {
			return -1;
		}
		len = snprintf(
		        mode_buf, sizeof(mode_buf), "%d",
		        listener->bubbles ? 1 : 0);
		if (len < 0) {
			return -1;
		}
		if (bud_buf_append(buf, mode_buf) != 0) {
			return -1;
		}
		first = 0;
	}

	return 0;
}

static int bud_render_html_node(const bud_node *node, bud_buf *buf)
{
	const bud_attr *attr;
	const bud_node *child;

	if (!node) {
		return 0;
	}

	switch (node->kind) {
	case BUD_NODE_FRAGMENT:
		for (child = node->first_child; child;
		     child = child->next_sibling)
		{
			if (bud_render_html_node(child, buf) != 0) {
				return -1;
			}
		}
		return 0;
	case BUD_NODE_TEXT:
		return bud_buf_append_escaped(buf, node->text, 0);
	case BUD_NODE_RAW_HTML:
		return bud_buf_append(buf, node->text);
	case BUD_NODE_COMPONENT: {
		bud_node *expanded = bud_ssr_expand_component(node);
		int rc = 0;
		if (expanded) {
			rc = bud_render_html_node(expanded, buf);
			bud_free(expanded);
		}
		return rc;
	}
	case BUD_NODE_ELEMENT:
		break;
	}

	if (bud_buf_append_char(buf, '<') != 0) {
		return -1;
	}
	if (bud_buf_append(buf, node->tag) != 0) {
		return -1;
	}
#ifdef BUD_DEBUG
	if (node->src_file && node->src_file[0]) {
		char src_buf[384];
		int src_len = snprintf(
		        src_buf, sizeof(src_buf), "%s:%d", node->src_file,
		        node->src_line);
		if (src_len > 0 && src_len < (int)sizeof(src_buf)) {
			if (bud_append_attr(buf, "data-bud-src", src_buf) != 0)
			{
				return -1;
			}
		}
	}
#endif
	for (attr = node->attrs; attr; attr = attr->next) {
		if (attr->is_boolean) {
			if (bud_buf_append_char(buf, ' ') != 0) {
				return -1;
			}
			if (bud_buf_append(buf, attr->name) != 0) {
				return -1;
			}
		} else {
			if (bud_append_attr(buf, attr->name, attr->value) != 0)
			{
				return -1;
			}
		}
	}
	if (bud_is_void_tag(node->tag)) {
		if (bud_buf_append(buf, " />") != 0) {
			return -1;
		}
		return 0;
	}
	if (bud_buf_append_char(buf, '>') != 0) {
		return -1;
	}
	for (child = node->first_child; child; child = child->next_sibling) {
		if (bud_render_html_node(child, buf) != 0) {
			return -1;
		}
	}
	if (bud_buf_append(buf, "</") != 0) {
		return -1;
	}
	if (bud_buf_append(buf, node->tag) != 0) {
		return -1;
	}
	if (bud_buf_append_char(buf, '>') != 0) {
		return -1;
	}
	return 0;
}

static int bud_render_hydrated_html_node(
        const bud_node *node, bud_buf *buf, unsigned int *next_id)
{
	const bud_attr *attr;
	const bud_node *child;
	char id_buf[32];
	unsigned int id;

	if (!node) {
		return 0;
	}

	if (node->kind == BUD_NODE_COMPONENT) {
		bud_node *expanded = bud_expand_node_once((bud_node *)node);
		int rc = 0;
		if (expanded) {
			rc = bud_render_hydrated_html_node(expanded, buf, next_id);
			bud_free(expanded);
		}
		return rc;
	}

	if (next_id) {
		id = (*next_id)++;
		((bud_node *)node)->id = id;
	} else {
		id = node->id;
	}

	if (snprintf(id_buf, sizeof(id_buf), "%u", id) < 0) {
		return -1;
	}

	switch (node->kind) {
	case BUD_NODE_FRAGMENT:
		if (bud_buf_append(buf, "<!--bud-fragment:") != 0) {
			return -1;
		}
		if (bud_buf_append(buf, id_buf) != 0) {
			return -1;
		}
		if (bud_buf_append(buf, "-->") != 0) {
			return -1;
		}
		for (child = node->first_child; child;
		     child = child->next_sibling)
		{
			if (bud_render_hydrated_html_node(child, buf, next_id) != 0) {
				return -1;
			}
		}
		if (bud_buf_append(buf, "<!--/bud-fragment:") != 0) {
			return -1;
		}
		if (bud_buf_append(buf, id_buf) != 0) {
			return -1;
		}
		if (bud_buf_append(buf, "-->") != 0) {
			return -1;
		}
		return 0;
	case BUD_NODE_TEXT:
		if (bud_buf_append(buf, "<!--bud-text:") != 0) {
			return -1;
		}
		if (bud_buf_append(buf, id_buf) != 0) {
			return -1;
		}
		if (bud_buf_append(buf, "-->") != 0) {
			return -1;
		}
		if (bud_buf_append_escaped(buf, node->text, 0) != 0) {
			return -1;
		}
		if (bud_buf_append(buf, "<!--/bud-text:") != 0) {
			return -1;
		}
		if (bud_buf_append(buf, id_buf) != 0) {
			return -1;
		}
		if (bud_buf_append(buf, "-->") != 0) {
			return -1;
		}
		return 0;
	case BUD_NODE_RAW_HTML:
		return bud_buf_append(buf, node->text);
	case BUD_NODE_COMPONENT:
		return 0;
	case BUD_NODE_ELEMENT:
		break;
	}

	if (bud_buf_append_char(buf, '<') != 0) {
		return -1;
	}
	if (bud_buf_append(buf, node->tag) != 0) {
		return -1;
	}
	if (bud_append_attr(buf, "data-bud-id", id_buf) != 0) {
		return -1;
	}
#ifdef BUD_DEBUG
	if (node->src_file && node->src_file[0]) {
		char src_buf[384];
		int src_len = snprintf(
		        src_buf, sizeof(src_buf), "%s:%d", node->src_file,
		        node->src_line);
		if (src_len > 0 && src_len < (int)sizeof(src_buf)) {
			if (bud_append_attr(buf, "data-bud-src", src_buf) != 0)
			{
				return -1;
			}
		}
	}
#endif
	if (node->listeners) {
		if (bud_buf_append(buf, " data-bud-on=\"") != 0) {
			return -1;
		}
		if (bud_listeners_to_attr(node, buf) != 0) {
			return -1;
		}
		if (bud_buf_append_char(buf, '"') != 0) {
			return -1;
		}
	}
	for (attr = node->attrs; attr; attr = attr->next) {
		if (attr->is_boolean) {
			if (bud_buf_append_char(buf, ' ') != 0) {
				return -1;
			}
			if (bud_buf_append(buf, attr->name) != 0) {
				return -1;
			}
		} else {
			if (bud_append_attr(buf, attr->name, attr->value) != 0)
			{
				return -1;
			}
		}
	}
	if (bud_is_void_tag(node->tag)) {
		if (bud_buf_append(buf, " />") != 0) {
			return -1;
		}
		return 0;
	}
	if (bud_buf_append_char(buf, '>') != 0) {
		return -1;
	}
	for (child = node->first_child; child; child = child->next_sibling) {
		if (bud_render_hydrated_html_node(child, buf, next_id) != 0) {
			return -1;
		}
	}
	if (bud_buf_append(buf, "</") != 0) {
		return -1;
	}
	if (bud_buf_append(buf, node->tag) != 0) {
		return -1;
	}
	if (bud_buf_append_char(buf, '>') != 0) {
		return -1;
	}
	return 0;
}

static int bud_render_ops_node(
        const bud_node *node, bud_emit_fn emit, void *user,
        unsigned int *next_id)
{
	const bud_attr *attr;
	const bud_listener *listener;
	const bud_node *child;
	char id_buf[32];
	char mode_buf[8];
	unsigned int id;
	int len;

	if (!node) {
		return 0;
	}

	if (node->kind == BUD_NODE_COMPONENT) {
		bud_node *expanded = bud_expand_node_once((bud_node *)node);
		int rc = 0;
		if (expanded) {
			rc = bud_render_ops_node(expanded, emit, user, next_id);
			bud_free(expanded);
		}
		return rc;
	}

	if (next_id) {
		id = (*next_id)++;
		((bud_node *)node)->id = id;
	} else {
		id = node->id;
	}

	len = snprintf(id_buf, sizeof(id_buf), "%u", id);
	if (len < 0) {
		return -1;
	}

	switch (node->kind) {
	case BUD_NODE_FRAGMENT:
		if (emit(user, "fragment-open", id_buf, NULL, NULL) != 0) {
			return -1;
		}
		for (child = node->first_child; child;
		     child = child->next_sibling)
		{
			if (bud_render_ops_node(child, emit, user, next_id) != 0) {
				return -1;
			}
		}
		return emit(user, "fragment-close", id_buf, NULL, NULL) != 0
		               ? -1
		               : 0;
	case BUD_NODE_ELEMENT:
		if (emit(user, "element-open", node->tag, id_buf, NULL) != 0) {
			return -1;
		}
		for (attr = node->attrs; attr; attr = attr->next) {
			if (emit(user, "attr", attr->name,
			         attr->value ? attr->value : "", id_buf) != 0)
			{
				return -1;
			}
		}
		for (listener = node->listeners; listener;
		     listener = listener->next)
		{
			snprintf(
			        mode_buf, sizeof(mode_buf), "%d",
			        listener->bubbles ? 1 : 0);
			if (emit(user, "listener", listener->event, mode_buf,
			         id_buf) != 0)
			{
				return -1;
			}
		}
		for (child = node->first_child; child;
		     child = child->next_sibling)
		{
			if (bud_render_ops_node(child, emit, user, next_id) != 0) {
				return -1;
			}
		}
		return emit(user, "element-close", node->tag, id_buf, NULL) != 0
		               ? -1
		               : 0;
	case BUD_NODE_TEXT:
		return emit(user, "text", node->text, id_buf, NULL) != 0 ? -1
		                                                         : 0;
	case BUD_NODE_RAW_HTML:
		return emit(user, "raw", node->text, id_buf, NULL) != 0 ? -1
		                                                        : 0;
	default:
		return -1;
	}
}

static int bud_hydrate_node(
        const bud_node *node, bud_hydrate_lookup_fn lookup, void *user,
        unsigned int *next_id)
{
	const bud_node *child;
	const char *kind;
	const char *tag;
	const char *text;
	unsigned int id;

	if (!node) {
		return 0;
	}

	if (node->kind == BUD_NODE_COMPONENT) {
		bud_node *expanded = bud_expand_node_once((bud_node *)node);
		int rc = 0;
		if (expanded) {
			rc = bud_hydrate_node(expanded, lookup, user, next_id);
			bud_free(expanded);
		}
		return rc;
	}

	if (next_id) {
		id = (*next_id)++;
		((bud_node *)node)->id = id;
	} else {
		id = node->id;
	}

	kind = NULL;
	tag = NULL;
	text = NULL;
	if (lookup(user, id, &kind, &tag, &text) != 0) {
		return -1;
	}

	switch (node->kind) {
	case BUD_NODE_FRAGMENT:
		if (!kind || strcmp(kind, "fragment") != 0) {
			return -1;
		}
		for (child = node->first_child; child;
		     child = child->next_sibling)
		{
			if (bud_hydrate_node(child, lookup, user, next_id) != 0) {
				return -1;
			}
		}
		return 0;
	case BUD_NODE_ELEMENT:
		if (!kind || strcmp(kind, "element") != 0) {
			return -1;
		}
		if (!tag || strcmp(tag, node->tag) != 0) {
			return -1;
		}
		for (child = node->first_child; child;
		     child = child->next_sibling)
		{
			if (bud_hydrate_node(child, lookup, user, next_id) != 0) {
				return -1;
			}
		}
		return 0;
	case BUD_NODE_TEXT:
		if (!kind || strcmp(kind, "text") != 0) {
			return -1;
		}
		if (!text || strcmp(text, node->text) != 0) {
			return -1;
		}
		return 0;
	case BUD_NODE_RAW_HTML:
		if (!kind || strcmp(kind, "raw") != 0) {
			return -1;
		}
		if (!text || strcmp(text, node->text) != 0) {
			return -1;
		}
		return 0;
	default:
		return -1;
	}
}

char *bud_render_html(const bud_node *root)
{
	bud_buf buf;

	bud_buf_init(&buf);
	if (bud_render_html_node(root, &buf) != 0) {
		free(buf.data);
		return NULL;
	}

	if (!buf.data) {
		buf.data = (char *)malloc(1);
		if (!buf.data) {
			return NULL;
		}
		buf.data[0] = '\0';
	}

	return buf.data;
}

char *bud_render_hydrated_html(const bud_node *root)
{
	bud_buf buf;
	unsigned int next_id = 0;

	bud_buf_init(&buf);
	if (bud_render_hydrated_html_node(
	            root, &buf, (root && root->ids_frozen) ? NULL : &next_id) != 0)
	{
		free(buf.data);
		return NULL;
	}

	if (!buf.data) {
		buf.data = (char *)malloc(1);
		if (!buf.data) {
			return NULL;
		}
		buf.data[0] = '\0';
	}

	return buf.data;
}

int bud_render_ops(const bud_node *root, bud_emit_fn emit, void *user)
{
	unsigned int next_id = 0;

	if (!emit) {
		return -1;
	}

	return bud_render_ops_node(
	        root, emit, user, (root && root->ids_frozen) ? NULL : &next_id);
}

int bud_render_hydration_ops(const bud_node *root, bud_emit_fn emit, void *user)
{
	unsigned int next_id = 0;

	if (!emit) {
		return -1;
	}

	return bud_render_ops_node(
	        root, emit, user, (root && root->ids_frozen) ? NULL : &next_id);
}

/* parent_id: when non-NULL the id of the live element this node is inserted
 * into. A diff that adds a node has to say where: the host cannot guess the
 * parent from whatever element its last patch happened to leave open. Only the
 * top node of an inserted subtree carries it; descendants nest on the host's
 * open-element stack as usual. */
static int bud_render_patch_ops_node_at(
        const bud_node *node, const char *parent_id, bud_emit_fn emit,
        void *user);

static int
bud_render_patch_ops_node(const bud_node *node, bud_emit_fn emit, void *user)
{
	return bud_render_patch_ops_node_at(node, NULL, emit, user);
}

static int bud_render_patch_ops_node_at(
        const bud_node *node, const char *parent_id, bud_emit_fn emit,
        void *user)
{
	const bud_attr *attr;
	const bud_listener *listener;
	const bud_node *child;
	char id_buf[32];
	char bubbles_buf[8];
	int len;

	if (!node) {
		return 0;
	}

	len = snprintf(id_buf, sizeof(id_buf), "%u", node->id);
	if (len < 0) {
		return -1;
	}

	switch (node->kind) {
	case BUD_NODE_FRAGMENT:
		if (emit(user, "patch-open", "fragment", id_buf, parent_id) != 0) {
			return -1;
		}
		for (child = node->first_child; child;
		     child = child->next_sibling)
		{
			if (bud_render_patch_ops_node(child, emit, user) != 0) {
				return -1;
			}
		}
		return emit(user, "patch-close", "fragment", id_buf, NULL) != 0
		               ? -1
		               : 0;
	case BUD_NODE_ELEMENT:
		if (emit(user, "patch-open", node->tag, id_buf, parent_id) != 0) {
			return -1;
		}
		for (attr = node->attrs; attr; attr = attr->next) {
			if (emit(user, "patch-attr", id_buf, attr->name,
			         attr->value ? attr->value : "") != 0)
			{
				return -1;
			}
		}
		for (listener = node->listeners; listener;
		     listener = listener->next)
		{
			snprintf(
			        bubbles_buf, sizeof(bubbles_buf), "%d",
			        listener->bubbles ? 1 : 0);
			if (emit(user, "patch-listener", id_buf,
			         listener->event, bubbles_buf) != 0)
			{
				return -1;
			}
		}
		for (child = node->first_child; child;
		     child = child->next_sibling)
		{
			if (bud_render_patch_ops_node(child, emit, user) != 0) {
				return -1;
			}
		}
		return emit(user, "patch-close", node->tag, id_buf, NULL) != 0
		               ? -1
		               : 0;
	case BUD_NODE_TEXT:
		return emit(user, "patch-text", node->text, id_buf, parent_id) != 0
		               ? -1
		               : 0;
	case BUD_NODE_RAW_HTML:
		return emit(user, "patch-raw", node->text, id_buf, parent_id) != 0
		               ? -1
		               : 0;
	default:
		return -1;
	}
}

int bud_render_patch_ops(const bud_node *root, bud_emit_fn emit, void *user)
{
	char id_buf[32];
	bud_node *tmp = NULL;
	const bud_node *eff;
	int len;
	int rc;

	if (!emit) {
		return -1;
	}

	if (!root) {
		return 0;
	}

	if (root->kind == BUD_NODE_COMPONENT) {
		tmp = bud_expand_tree(bud_expand_node_once((bud_node *)root));
		eff = tmp;
	} else {
		eff = root;
	}
	if (!eff) {
		return 0;
	}
	bud_prepare_render((bud_node *)eff);
	len = snprintf(id_buf, sizeof(id_buf), "%u", eff->id);
	if (len < 0) {
		bud_free(tmp);
		return -1;
	}
	if (emit(user, "patch-clear", id_buf, NULL, NULL) != 0) {
		bud_free(tmp);
		return -1;
	}
	rc = bud_render_patch_ops_node(eff, emit, user);
	bud_free(tmp);
	return rc;
}

static const char *bud_get_node_key(const bud_node *node)
{
	if (!node || node->kind != BUD_NODE_ELEMENT)
		return NULL;
	const bud_attr *attr = bud_attr_find((bud_node *)node, "data-key");
	if (attr && attr->value && attr->value[0])
		return attr->value;
	attr = bud_attr_find((bud_node *)node, "data-bud-key");
	if (attr && attr->value && attr->value[0])
		return attr->value;
	attr = bud_attr_find((bud_node *)node, "key");
	if (attr && attr->value && attr->value[0])
		return attr->value;
	return NULL;
}

/* Ids in a diffed tree are stable, not positional. Matched nodes keep the id
 * the DOM already carries; every node the diff introduces (inserted or
 * replaced subtrees) takes a fresh id above everything the old tree used, so
 * it can never collide with a live node. Renumbering DFS after a structural
 * change (as the first version did) silently re-pointed later nodes at the
 * wrong DOM elements. */
static unsigned int bud_diff_next_id;

#define BUD_NO_PARENT ((unsigned int)-1)

static unsigned int bud_max_id(const bud_node *node)
{
	unsigned int m = node ? node->id : 0;
	for (const bud_node *c = node ? node->first_child : NULL; c;
	     c = c->next_sibling)
	{
		unsigned int cm = bud_max_id(c);
		if (cm > m)
			m = cm;
	}
	return m;
}

static void bud_assign_fresh_ids(bud_node *node)
{
	if (!node)
		return;
	node->id = bud_diff_next_id++;
	for (bud_node *c = node->first_child; c; c = c->next_sibling)
		bud_assign_fresh_ids(c);
}

static int bud_vdom_diff_node(
        bud_node *old_node, bud_node *new_node, unsigned int parent_id,
        bud_emit_fn emit, void *user)
{
	char id_buf[32];
	const bud_attr *new_attr, *old_attr;
	const bud_node *old_child, *new_child;
	int len;

	if (!old_node && !new_node)
		return 0;

	if (!old_node && new_node) {
		char parent_buf[32];
		bud_assign_fresh_ids(new_node);
		if (parent_id == BUD_NO_PARENT)
			return bud_render_patch_ops_node(new_node, emit, user);
		snprintf(parent_buf, sizeof(parent_buf), "%u", parent_id);
		return bud_render_patch_ops_node_at(
		        new_node, parent_buf, emit, user);
	}

	if (old_node && !new_node) {
		len = snprintf(id_buf, sizeof(id_buf), "%u", old_node->id);
		if (len < 0)
			return -1;
		return emit(user, "patch-remove", id_buf, NULL, NULL);
	}

	new_node->id = old_node->id;
	len = snprintf(id_buf, sizeof(id_buf), "%u", old_node->id);
	if (len < 0)
		return -1;

	if (old_node->kind != new_node->kind ||
	    (old_node->kind == BUD_NODE_ELEMENT &&
	     strcmp(old_node->tag, new_node->tag) != 0))
	{
		for (bud_node *c = new_node->first_child; c; c = c->next_sibling)
			bud_assign_fresh_ids(c);
		bud_buf b;
		bud_buf_init(&b);
		if (bud_render_hydrated_html_node(new_node, &b, NULL) != 0) {
			free(b.data);
			return -1;
		}
		int rc = emit(user, "patch-replace", id_buf, b.data ? b.data : "", NULL);
		free(b.data);
		return rc;
	}

	if (old_node->kind == BUD_NODE_TEXT) {
		const char *ot = old_node->text ? old_node->text : "";
		const char *nt = new_node->text ? new_node->text : "";
		if (strcmp(ot, nt) != 0) {
			return emit(user, "patch-text", nt, id_buf, NULL);
		}
		return 0;
	}

	if (old_node->kind == BUD_NODE_RAW_HTML) {
		const char *ot = old_node->text ? old_node->text : "";
		const char *nt = new_node->text ? new_node->text : "";
		if (strcmp(ot, nt) != 0) {
			return emit(user, "patch-innerhtml", id_buf, nt, NULL);
		}
		return 0;
	}

	if (old_node->kind == BUD_NODE_ELEMENT) {
		for (new_attr = new_node->attrs; new_attr;
		     new_attr = new_attr->next)
		{
			old_attr = bud_attr_find(old_node, new_attr->name);
			const char *nv = new_attr->value
			        ? new_attr->value
			        : (new_attr->is_boolean ? new_attr->name : "");
			const char *ov = old_attr
			        ? (old_attr->value ? old_attr->value
			                           : (old_attr->is_boolean
			                                      ? old_attr->name
			                                      : ""))
			        : NULL;
			if (!old_attr || strcmp(ov, nv) != 0)
			{
				if (emit(user, "patch-attr", id_buf,
				         new_attr->name, nv) != 0)
					return -1;
			}
		}
		for (old_attr = old_node->attrs; old_attr;
		     old_attr = old_attr->next)
		{
			if (!bud_attr_find(new_node, old_attr->name)) {
				if (emit(user, "patch-attr", id_buf,
				         old_attr->name, "") != 0)
					return -1;
			}
		}

		/* Emit patch-listener only for newly added listeners */
		const bud_listener *nl;
		for (nl = new_node->listeners; nl; nl = nl->next) {
			const bud_listener *ol;
			int already = 0;
			for (ol = old_node->listeners; ol; ol = ol->next) {
				if (ol->event && nl->event &&
				    strcmp(ol->event, nl->event) == 0 &&
				    (ol->bubbles ? 1 : 0) == (nl->bubbles ? 1 : 0)) {
					already = 1;
					break;
				}
			}
			if (!already) {
				char bubbles_buf[8];
				snprintf(bubbles_buf, sizeof(bubbles_buf), "%d", nl->bubbles ? 1 : 0);
				if (emit(user, "patch-listener", id_buf, nl->event, bubbles_buf) != 0)
					return -1;
			}
		}

		/* Elements marked as foreign or terminal manage their own children
		 * (e.g. xterm.js or WebGL canvas); skip diffing their VDOM children. */
		if ((bud_attr_find(old_node, "data-bud-foreign") ||
		     bud_attr_find(old_node, "data-bud-terminal")) &&
		    (bud_attr_find(new_node, "data-bud-foreign") ||
		     bud_attr_find(new_node, "data-bud-terminal")))
		{
			return 0;
		}

		/* If this element contains raw HTML, emit patch-innerhtml on
		 * this element directly */
		if (old_node->first_child &&
		    old_node->first_child->kind == BUD_NODE_RAW_HTML &&
		    new_node->first_child &&
		    new_node->first_child->kind == BUD_NODE_RAW_HTML &&
		    !old_node->first_child->next_sibling &&
		    !new_node->first_child->next_sibling)
		{
			const char *ot = old_node->first_child->text
			                         ? old_node->first_child->text
			                         : "";
			const char *nt = new_node->first_child->text
			                         ? new_node->first_child->text
			                         : "";
			/* The raw child is never visited below: give it the id of the
			 * one it replaces so no stale DFS number is left behind. */
			new_node->first_child->id = old_node->first_child->id;
			if (strcmp(ot, nt) != 0) {
				return emit(
				        user, "patch-innerhtml", id_buf, nt,
				        NULL);
			}
			return 0;
		}
	}

	int has_keys = 0;
	for (const bud_node *c = old_node->first_child; c; c = c->next_sibling)
	{
		if (bud_get_node_key(c)) {
			has_keys = 1;
			break;
		}
	}
	if (!has_keys) {
		for (const bud_node *c = new_node->first_child; c;
		     c = c->next_sibling)
		{
			if (bud_get_node_key(c)) {
				has_keys = 1;
				break;
			}
		}
	}

	if (has_keys) {
#define BUD_MAX_DIFF_CHILDREN 256
		const bud_node *old_children[BUD_MAX_DIFF_CHILDREN];
		int matched[BUD_MAX_DIFF_CHILDREN] = { 0 };
		int n_old = 0;

		for (const bud_node *c = old_node->first_child;
		     c && n_old < BUD_MAX_DIFF_CHILDREN; c = c->next_sibling)
		{
			old_children[n_old++] = c;
		}

		for (const bud_node *nc = new_node->first_child; nc;
		     nc = nc->next_sibling)
		{
			const char *nkey = bud_get_node_key(nc);
			int found_idx = -1;
			if (nkey) {
				for (int i = 0; i < n_old; i++) {
					if (!matched[i]) {
						const char *okey =
						        bud_get_node_key(
						                old_children
						                        [i]);
						if (okey &&
						    strcmp(okey, nkey) == 0)
						{
							found_idx = i;
							break;
						}
					}
				}
			} else {
				for (int i = 0; i < n_old; i++) {
					if (!matched[i] &&
					    !bud_get_node_key(old_children[i]))
					{
						if (old_children[i]->kind == nc->kind &&
						    (nc->kind != BUD_NODE_ELEMENT ||
						     (old_children[i]->tag && nc->tag &&
						      strcmp(old_children[i]->tag, nc->tag) == 0)))
						{
							found_idx = i;
							break;
						}
					}
				}
				if (found_idx < 0) {
					for (int i = 0; i < n_old; i++) {
						if (!matched[i] &&
						    !bud_get_node_key(old_children[i]))
						{
							found_idx = i;
							break;
						}
					}
				}
			}

			if (found_idx >= 0) {
				matched[found_idx] = 1;
				if (bud_vdom_diff_node(
				            (bud_node *)old_children[found_idx],
				            (bud_node *)nc, old_node->id, emit,
				            user) != 0)
					return -1;
			} else {
				if (bud_vdom_diff_node(
				            NULL, (bud_node *)nc, old_node->id,
				            emit, user) != 0)
					return -1;
			}
		}

		for (int i = 0; i < n_old; i++) {
			if (!matched[i]) {
				if (bud_vdom_diff_node(
				            (bud_node *)old_children[i], NULL,
				            old_node->id, emit, user) != 0)
					return -1;
			}
		}
		return 0;
	}

	old_child = old_node->first_child;
	new_child = new_node->first_child;
	while (old_child || new_child) {
		const bud_node *next_old =
		        old_child ? old_child->next_sibling : NULL;
		const bud_node *next_new =
		        new_child ? new_child->next_sibling : NULL;
		if (bud_vdom_diff_node(
		            (bud_node *)old_child, (bud_node *)new_child,
		            old_node->id, emit, user) != 0)
			return -1;
		old_child = next_old;
		new_child = next_new;
	}

	return 0;
}

int bud_vdom_diff(
        bud_node *old_root, bud_node *new_root, bud_emit_fn emit, void *user)
{
	bud_node *old_tmp = NULL;
	bud_node *new_tmp = NULL;
	bud_node *eff_old = old_root;
	bud_node *eff_new = new_root;
	int rc;

	if (!emit)
		return -1;
	if (!old_root && !new_root)
		return 0;

	if (eff_old) {
		if (eff_old->kind == BUD_NODE_COMPONENT) {
			old_tmp = bud_expand_tree(bud_expand_node_once(eff_old));
			eff_old = old_tmp;
		} else {
			bud_expand_children(eff_old);
		}
		if (eff_old)
			bud_prepare_render(eff_old);
	}
	if (eff_new) {
		if (eff_new->kind == BUD_NODE_COMPONENT) {
			new_tmp = bud_expand_tree(bud_expand_node_once(eff_new));
			eff_new = new_tmp;
		} else {
			bud_expand_children(eff_new);
		}
		if (eff_new)
			bud_prepare_render(eff_new);
	}
	bud_diff_next_id = eff_old ? bud_max_id(eff_old) + 1 : 0;
	rc = bud_vdom_diff_node(eff_old, eff_new, BUD_NO_PARENT, emit, user);
	if (new_root && !new_tmp && rc == 0)
		new_root->ids_frozen = 1;
	bud_free(old_tmp);
	bud_free(new_tmp);
	return rc;
}

int bud_hydrate(const bud_node *root, bud_hydrate_lookup_fn lookup, void *user)
{
	unsigned int next_id = 0;

	if (!lookup) {
		return -1;
	}

	return bud_hydrate_node(
	        root, lookup, user, (root && root->ids_frozen) ? NULL : &next_id);
}

/* ── Tree dump for debugging ── */

static int bud_sprint_tree_node(
        const bud_node *node, char *buf, size_t bufsz, size_t *pos, int depth)
{
	const bud_attr *attr;
	size_t i;
	int n;

	if (!node)
		return 0;

	/* indent */
	for (i = 0; i < (size_t)depth && *pos < bufsz; i++) {
		buf[*pos] = ' ';
		(*pos)++;
	}
	if (*pos >= bufsz)
		return -1;

	/* id= */
	n = snprintf(buf + *pos, bufsz - *pos, "id=%u ", bud_node_id(node));
	if (n < 0 || (size_t)n >= bufsz - *pos)
		return -1;
	*pos += n;

	/* kind */
	switch (bud_node_kind_of(node)) {
	case BUD_NODE_FRAGMENT:
		n = snprintf(buf + *pos, bufsz - *pos, "kind=FRAGMENT");
		break;
	case BUD_NODE_ELEMENT:
		n = snprintf(buf + *pos, bufsz - *pos, "kind=ELEMENT");
		break;
	case BUD_NODE_TEXT:
		n = snprintf(buf + *pos, bufsz - *pos, "kind=TEXT");
		break;
	case BUD_NODE_RAW_HTML:
		n = snprintf(buf + *pos, bufsz - *pos, "kind=RAW_HTML");
		break;
	case BUD_NODE_COMPONENT:
		n = snprintf(buf + *pos, bufsz - *pos, "kind=COMPONENT");
		break;
	default:
		n = snprintf(buf + *pos, bufsz - *pos, "kind=?");
		break;
	}
	if (n < 0 || (size_t)n >= bufsz - *pos)
		return -1;
	*pos += n;

	/* tag (for elements) */
	if (bud_node_kind_of(node) == BUD_NODE_ELEMENT && bud_node_tag(node)) {
		n = snprintf(
		        buf + *pos, bufsz - *pos, " tag=%s",
		        bud_node_tag(node));
		if (n < 0 || (size_t)n >= bufsz - *pos)
			return -1;
		*pos += n;

		/* first two key attrs: id, class */
		for (attr = node->attrs; attr; attr = attr->next) {
			if (strcmp(attr->name, "id") == 0 ||
			    strcmp(attr->name, "class") == 0)
			{
				n = snprintf(
				        buf + *pos, bufsz - *pos, " %s=\"%s\"",
				        attr->name, attr->value);
				if (n < 0 || (size_t)n >= bufsz - *pos)
					return -1;
				*pos += n;
			}
		}
	}

	/* text snippet (for TEXT) */
	if (bud_node_kind_of(node) == BUD_NODE_TEXT && bud_node_text(node)) {
		const char *t = bud_node_text(node);
		size_t tlen = strlen(t);
		n = snprintf(
		        buf + *pos, bufsz - *pos, " \"%.*s%s\"",
		        (int)(tlen < 40 ? tlen : 40), t,
		        tlen > 40 ? "..." : "");
		if (n < 0 || (size_t)n >= bufsz - *pos)
			return -1;
		*pos += n;
	}

	/* source location */
#ifdef BUD_DEBUG
	if (node->src_file) {
		n = snprintf(
		        buf + *pos, bufsz - *pos, " [%s:%d]", node->src_file,
		        node->src_line);
		if (n < 0 || (size_t)n >= bufsz - *pos)
			return -1;
		*pos += n;
	}
#endif

	/* newline */
	if (*pos < bufsz) {
		buf[*pos] = '\n';
		(*pos)++;
	}

	/* children */
	for (i = 0; i < bud_node_child_count(node); i++) {
		if (bud_sprint_tree_node(
		            bud_node_child(node, i), buf, bufsz, pos,
		            depth + 1) != 0)
			return -1;
	}

	return 0;
}

int bud_sprint_tree(const bud_node *root, char *buf, size_t bufsz)
{
	size_t pos = 0;

	if (!root || !buf || bufsz == 0)
		return -1;

	buf[0] = '\0';
	if (bud_sprint_tree_node(root, buf, bufsz, &pos, 0) != 0)
		return -1;

	if (pos < bufsz)
		buf[pos] = '\0';
	else
		buf[bufsz - 1] = '\0';

	return 0;
}
