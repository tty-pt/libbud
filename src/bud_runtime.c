#include "bud_internal.h"

bud_runtime *bud_runtime_new(bud_node *root)
{
	bud_runtime *runtime;

	runtime = (bud_runtime *)calloc(1, sizeof(*runtime));
	if (!runtime) {
		return NULL;
	}

	runtime->root = root;
	return runtime;
}

void bud_runtime_free(bud_runtime *runtime)
{
	if (!runtime) {
		return;
	}

	bud_runtime_unmount(runtime);
	free(runtime);
}

bud_node *bud_runtime_root(const bud_runtime *runtime)
{
	if (!runtime) {
		return NULL;
	}

	return runtime->root;
}

int bud_runtime_set_invalidate(
        bud_runtime *runtime, bud_runtime_invalidate_fn fn, void *user)
{
	if (!runtime) {
		return -1;
	}

	runtime->invalidate = fn;
	runtime->invalidate_user = user;
	return 0;
}

int bud_runtime_mark_dirty(bud_runtime *runtime)
{
	if (!runtime) {
		return -1;
	}

	runtime->dirty = 1;
	return 0;
}

int bud_runtime_is_dirty(const bud_runtime *runtime)
{
	if (!runtime) {
		return 0;
	}

	return runtime->dirty;
}

int bud_runtime_flush(bud_runtime *runtime)
{
	if (!runtime) {
		return -1;
	}

	if (!runtime->dirty) {
		return 0;
	}

	runtime->dirty = 0;
	if (runtime->invalidate) {
		runtime->invalidate(runtime->invalidate_user, runtime);
	}

	return 0;
}

int bud_runtime_mount(bud_runtime *runtime)
{
	if (!runtime) {
		return -1;
	}

	if (runtime->mounted) {
		return 0;
	}

	runtime->mounted = 1;
	bud_node_call_mount(runtime->root, runtime);
	return 0;
}

int bud_runtime_update(bud_runtime *runtime)
{
	if (!runtime) {
		return -1;
	}

	if (!runtime->mounted) {
		return bud_runtime_mount(runtime);
	}

	bud_node_call_update(runtime->root, runtime);
	return 0;
}

int bud_runtime_unmount(bud_runtime *runtime)
{
	if (!runtime) {
		return -1;
	}

	if (!runtime->mounted) {
		return 0;
	}

	bud_node_call_unmount(runtime->root, runtime);
	runtime->mounted = 0;
	return 0;
}

static void bud_event_stop_default(bud_event *event)
{
	if (!event) {
		return;
	}

	event->stopped = 1;
}

void bud_event_stop_propagation(bud_event *event)
{
	bud_event_stop_default(event);
}

void bud_event_prevent_default(bud_event *event)
{
	if (!event) {
		return;
	}

	event->default_prevented = 1;
}

static int bud_dispatch_event_node(
        bud_node *node, bud_event *event, const char *listener_event)
{
	bud_listener *listener;
	int allow_bubble;

	if (!node) {
		return 0;
	}

	event->current_target = node;
	allow_bubble = node != event->target;
	for (listener = node->listeners; listener; listener = listener->next) {
		if (strcmp(listener->event, listener_event) != 0) {
			continue;
		}
		if (allow_bubble && !listener->bubbles) {
			continue;
		}
		if (listener->handler) {
			void *saved_user = event->user;
			int rc;
			if (listener->user) {
				event->user = listener->user;
			}
			rc = listener->handler(event);
			event->user = saved_user;
			if (rc != 0) {
				return -1;
			}
		}
		if (event->stopped) {
			return 0;
		}
	}

	return 0;
}

int bud_runtime_dispatch(
        bud_runtime *runtime, bud_node *target, const char *event,
        void *event_user)
{
	bud_event ev;
	bud_node *current;
	char *logical_event;
	char *target_suffix;
	int rc;

	if (!runtime || !target || !event) {
		return -1;
	}
	logical_event = malloc(strlen(event) + 1);
	if (!logical_event)
		return -1;
	strcpy(logical_event, event);
	target_suffix = strchr(logical_event, '@');
	if (target_suffix)
		*target_suffix = '\0';

	memset(&ev, 0, sizeof(ev));
	ev.type = logical_event;
	ev.target = target;
	ev.current_target = target;
	ev.user = event_user;
	ev.bubbles = 1;

	current = target;
	rc = 0;
	while (current) {
		if (bud_dispatch_event_node(current, &ev, event) != 0) {
			rc = -1;
			break;
		}
		if (!ev.bubbles || ev.stopped) {
			break;
		}
		current = current->parent;
	}

	free(logical_event);
	return rc;
}

/* ── Form-aware API action handler ──────────────────────────────────── */

/* Get the current value of a single form control from the Bud DOM tree.
   Returns a pointer to the attribute value (do not free). */
static const char *bud_control_value(bud_node *node)
{
	const char *tag;
	bud_node *child;

	if (!node || node->kind != BUD_NODE_ELEMENT)
		return NULL;
	tag = node->tag;
	if (!tag)
		return NULL;

	if (strcmp(tag, "select") == 0) {
		for (child = node->first_child; child;
		     child = child->next_sibling)
		{
			if (child->kind == BUD_NODE_ELEMENT && child->tag &&
			    strcmp(child->tag, "option") == 0 &&
			    bud_attr_find(child, "selected"))
			{
				const char *v = bud_get_attr(child, "value");
				return v ? v : "";
			}
		}
		return NULL;
	}

	if (strcmp(tag, "input") == 0) {
		const char *type = bud_get_attr(node, "type");
		if (type && strcmp(type, "checkbox") == 0)
			return bud_attr_find(node, "checked") ? "1" : "0";
		return bud_get_attr(node, "value");
	}

	return NULL;
}

/* Recursively walk node children and collect name=value pairs from every
   named form control into the query buffer.  For the event-target control,
   use event->user (the live JS value) rather than the stale Bud DOM value,
   unless a pre-handler has updated the Bud DOM attribute. */
static void bud_collect_form_query(
        bud_node *node, bud_node *target, const char *target_val, char *buf,
        size_t buf_sz, int *pos)
{
	const char *tag;
	const char *type;
	const char *name;
	const char *value;
	bud_node *child;
	int n;

	if (!node || node->kind != BUD_NODE_ELEMENT)
		return;
	tag = node->tag;
	if (!tag)
		goto recurse;

	if (strcmp(tag, "select") == 0 || strcmp(tag, "input") == 0) {
		type = strcmp(tag, "input") == 0 ? bud_get_attr(node, "type")
		                                 : NULL;
		if (type && (strcmp(type, "hidden") == 0 ||
		             strcmp(type, "submit") == 0))
			goto recurse;

		name = bud_get_attr(node, "name");
		if (!name)
			goto recurse;

		if (node == target) {
			/* Prefer the live event value (event->user) — the Bud
			 * DOM reflects render-time state and is stale for the
			 * target control (the browser, not Bud, updates the
			 * real DOM on user interaction).  Fall back to Bud DOM
			 * only when the live value is absent. */
			value = target_val;
			if (!value || !value[0])
				value = bud_control_value(node);
		} else {
			value = bud_control_value(node);
		}

		if (value && value[0]) {
			n = snprintf(
			        buf + *pos, buf_sz - *pos, "%s%s=%s",
			        *pos > 0 ? "&" : "", name, value);
			if (n > 0 && (size_t)n < buf_sz - *pos)
				*pos += n;
		}
		return;
	}

recurse:
	for (child = node->first_child; child; child = child->next_sibling)
		bud_collect_form_query(
		        child, target, target_val, buf, buf_sz, pos);
}

int bud_api_action_handler(bud_event *event)
{
	bud_node *curr = event->target;
	const char *action = NULL;

	while (curr) {
		action = bud_get_attr(curr, "data-action");
		if (action)
			break;
		curr = curr->parent;
	}

	if (!action || !bud_host_fetch_fn)
		return 0;

	/* Find enclosing <form> element */
	curr = event->target;
	while (curr && curr->kind == BUD_NODE_ELEMENT) {
		if (curr->tag && strcmp(curr->tag, "form") == 0)
			break;
		curr = curr->parent;
	}

	char url[2048];

	if (curr && curr->kind == BUD_NODE_ELEMENT && curr->tag &&
	    strcmp(curr->tag, "form") == 0)
	{
		char query[1024];
		int pos = 0;

		bud_collect_form_query(
		        curr, event->target, (const char *)event->user, query,
		        sizeof(query), &pos);

		if (pos > 0) {
			snprintf(
			        url, sizeof(url), "%s%c%s", action,
			        strchr(action, '?') ? '&' : '?', query);
			bud_host_fetch_fn(url, strlen(url), 1);

			/* Sync browser location via form's action path */
			if (bud_host_set_location_fn) {
				const char *path = bud_get_attr(curr, "action");
				if (path) {
					snprintf(
					        url, sizeof(url), "%s?%s", path,
					        query);
					bud_host_set_location_fn(
					        url, strlen(url));
				}
			}
		} else {
			bud_host_fetch_fn(action, strlen(action), 1);
		}
	} else {
		/* No parent form — original single-value behaviour */
		const char *value = (const char *)event->user;
		if (value && value[0]) {
			snprintf(
			        url, sizeof(url), "%s%c%s", action,
			        strchr(action, '?') ? '&' : '?', value);
		} else {
			snprintf(url, sizeof(url), "%s", action);
		}
		bud_host_fetch_fn(url, strlen(url), 1);
	}

	return 0;
}

/* --- Centralized Composable Slot Architecture (bud_slot) --- */

#include "../include/bud/bud_slot.h"

#define BUD_MAX_SLOT_CONTRIBS 128
static const bud_slot_contrib_t *g_slot_contribs[BUD_MAX_SLOT_CONTRIBS];
static size_t g_slot_contrib_count = 0;

int bud_slot_register(const bud_slot_contrib_t *contrib)
{
	if (!contrib || g_slot_contrib_count >= BUD_MAX_SLOT_CONTRIBS)
		return -1;

	/* Insert in ascending priority order */
	size_t i = 0;
	while (i < g_slot_contrib_count && g_slot_contribs[i]->priority <= contrib->priority) {
		i++;
	}

	for (size_t j = g_slot_contrib_count; j > i; j--) {
		g_slot_contribs[j] = g_slot_contribs[j - 1];
	}
	g_slot_contribs[i] = contrib;
	g_slot_contrib_count++;
	return 0;
}

bud_node *bud_slot_render(int slot_id, const void *state)
{
	for (size_t i = 0; i < g_slot_contrib_count; i++) {
		if (g_slot_contribs[i]->slot_id == slot_id && g_slot_contribs[i]->render) {
			bud_node *n = g_slot_contribs[i]->render(slot_id, state);
			if (n)
				return n;
		}
	}
	return NULL;
}

int bud_slot_fill(int slot_id, bud_node *parent, const void *state)
{
	if (!parent)
		return -1;
	int filled = 0;
	for (size_t i = 0; i < g_slot_contrib_count; i++) {
		if (g_slot_contribs[i]->slot_id == slot_id && g_slot_contribs[i]->fill) {
			if (g_slot_contribs[i]->fill(slot_id, parent, state) == 0)
				filled++;
		}
	}
	return filled;
}

void bud_slot_reset(void)
{
	g_slot_contrib_count = 0;
}

#ifdef _WIN32
#define BUD_WEAK
#else
#define BUD_WEAK __attribute__((weak))
#endif

BUD_WEAK void bud_patch_attr(bud_node *node, const char *name, const char *value)
{
	if (!node || !name || !value)
		return;
	bud_set_attr(node, name, value);
}

BUD_WEAK void bud_patch_text(bud_node *node, const char *value)
{
	if (!node || !value)
		return;
	free(node->text);
	node->text = strdup(value);
}

BUD_WEAK void bud_app_set_state(void)
{
}


