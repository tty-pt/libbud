#ifndef BUD_BUD_H
#define BUD_BUD_H

/**
 * @file bud.h
 * @brief HTML tree builder and patch runtime.
 *
 * Builds DOM nodes in C and reconciles them against a live document via
 * minimal patch operations; pure and isomorphic (SSR + WASM).
 */

#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Node kinds in a bud tree.
 */
typedef enum bud_node_kind {
	/** Document fragment grouping child nodes without an element wrapper. */
	BUD_NODE_FRAGMENT = 0,
	/** Element node with a tag name. */
	BUD_NODE_ELEMENT = 1,
	/** Text node whose contents are HTML-escaped on render. */
	BUD_NODE_TEXT = 2,
	/** Raw HTML node emitted verbatim without escaping. */
	BUD_NODE_RAW_HTML = 3
} bud_node_kind;

/**
 * @brief Opaque DOM node handle.
 */
typedef struct bud_node bud_node;

#ifdef BUD_DEBUG
/** @brief Debug-only source-location fields stamped into bud_node. */
#define BUD_SRC_FIELDS                                                         \
	const char *src_file;                                                  \
	int src_line;
#else
/** @brief Empty placeholder when BUD_DEBUG is not defined. */
#define BUD_SRC_FIELDS
#endif
/**
 * @brief Opaque runtime handle managing a node tree lifecycle.
 */
typedef struct bud_runtime bud_runtime;

/**
 * @brief Callback invoked per emitted patch operation.
 *
 * @param[in] user Arbitrary caller data.
 * @param[in] op   Operation name (e.g. "append", "set_attr").
 * @param[in] a    First operand.
 * @param[in] b    Second operand.
 * @param[in] c    Third operand.
 * @return Non-zero to abort emission.
 */
typedef int (*bud_emit_fn)(
        void *user, const char *op, const char *a, const char *b,
        const char *c);

/**
 * @brief Callback resolving a hydrated node id to its DOM metadata.
 *
 * @param[in]  user  Arbitrary caller data.
 * @param[in]  id    Node id.
 * @param[out] kind  Receives the node kind.
 * @param[out] tag   Receives the element tag.
 * @param[out] text  Receives the text content.
 * @return Non-zero on success.
 */
typedef int (*bud_hydrate_lookup_fn)(
        void *user, unsigned int id, const char **kind, const char **tag,
        const char **text);

/**
 * @brief Callback invoked when the runtime is invalidated.
 *
 * @param[in] user    Arbitrary caller data.
 * @param[in] runtime Invalidated runtime.
 */
typedef void (*bud_runtime_invalidate_fn)(void *user, bud_runtime *runtime);

/**
 * @brief A DOM event dispatched through a bud runtime.
 */
typedef struct bud_event {
	/** Event type name. */
	const char *type;
	/** Node that received the event. */
	bud_node *target;
	/** Node currently being dispatched to. */
	bud_node *current_target;
	/** Arbitrary user data attached at dispatch. */
	void *user;
	/** Non-zero when the event bubbles up the tree. */
	int bubbles;
	/** Non-zero after stop_propagation. */
	int stopped;
	/** Non-zero after prevent_default. */
	int default_prevented;
} bud_event;

/**
 * @brief Event handler signature.
 *
 * @param[in] event Event being dispatched.
 * @return Non-zero to stop propagation.
 */
typedef int (*bud_event_handler_fn)(bud_event *event);

/**
 * @brief Node lifecycle callback.
 *
 * @param[in] user    Arbitrary caller data.
 * @param[in] runtime Owning runtime.
 * @param[in] node    Node entering/leaving the tree.
 */
typedef void (*bud_lifecycle_fn)(
        void *user, bud_runtime *runtime, const bud_node *node);

/**
 * @brief Component render function signature.
 *
 * @param[in] ctx   Component context.
 * @param[in] props Immutable props.
 * @return The rendered node subtree.
 */
typedef bud_node *(*bud_component_fn)(void *ctx, const void *props);

/**
 * @brief A renderable component.
 */
typedef struct bud_component {
	/** Render function. */
	bud_component_fn render;
	/** Component context. */
	void *ctx;
} bud_component;

/**
 * @brief Create an empty fragment node.
 * @return New fragment node.
 */
bud_node *bud_fragment(void);

/**
 * @brief Create an element node.
 * @param[in] tag Element tag name.
 * @return New element node.
 */
bud_node *bud_element(const char *tag);

/**
 * @brief Create a text node (HTML-escaped on render).
 * @param[in] text Text content.
 * @return New text node.
 */
bud_node *bud_text(const char *text);

/**
 * @brief Create a text node from a printf-style format.
 * @param[in] fmt Format string.
 * @return New text node.
 */
bud_node *bud_textf(const char *fmt, ...);

/**
 * @brief Create a hidden input element carrying a name/value pair.
 * @param[in] name Input name.
 * @param[in] value Input value.
 * @return New input element node.
 */
bud_node *bud_hidden_input(const char *name, const char *value);

/**
 * @brief Create a hidden input element carrying an integer value.
 * @param[in] name Input name.
 * @param[in] value Integer value (serialized as text).
 * @return New input element node.
 */
bud_node *bud_hidden_input_int(const char *name, int value);

/**
 * @brief Create a submit button element with an optional class.
 * @param[in] label      Button label.
 * @param[in] class_name CSS class, or NULL.
 * @return New button element node.
 */
bud_node *bud_submit_btn(const char *label, const char *class_name);

/**
 * @brief Create an anchor element.
 * @param[in] href       Target URL.
 * @param[in] text       Link text.
 * @param[in] class_name CSS class, or NULL.
 * @return New anchor element node.
 */
bud_node *bud_link(const char *href, const char *text, const char *class_name);

/**
 * @brief Create a raw-HTML node emitted verbatim (no escaping).
 * @param[in] html Raw markup.
 * @return New raw-HTML node.
 */
bud_node *bud_raw(const char *html);

/**
 * @brief Create a raw-HTML node from a printf-style format.
 * @param[in] fmt Format string.
 * @return New raw-HTML node.
 */
bud_node *bud_tpl(const char *fmt, ...);

/**
 * @brief Create a raw-HTML node from a format and va_list.
 * @param[in] fmt Format string.
 * @param[in] ap  Argument list.
 * @return New raw-HTML node.
 */
bud_node *bud_vtpl(const char *fmt, va_list ap);

/**
 * @brief Replace a raw node's text without touching child nodes.
 * @param[in] node Root node.
 * @param[in] text Replacement text.
 */
void bud_raw_set_text(bud_node *node, const char *text);

/**
 * @brief Render a component instance.
 * @param[in] component Component to render.
 * @param[in] props     Immutable props.
 * @return Rendered node subtree.
 */
bud_node *
bud_component_render(const bud_component *component, const void *props);

/**
 * @brief Set an attribute on a node.
 * @param[in] node  Target node.
 * @param[in] name  Attribute name.
 * @param[in] value Attribute value.
 * @return 0 on success.
 */
int bud_set_attr(bud_node *node, const char *name, const char *value);

/**
 * @brief Set an attribute from a printf-style format.
 * @param[in] node Target node.
 * @param[in] name Attribute name.
 * @param[in] fmt  Format string.
 * @return 0 on success.
 */
int bud_set_attr_fmt(bud_node *node, const char *name, const char *fmt, ...);

/**
 * @brief Set a boolean (valueless) attribute on a node.
 * @param[in] node Target node.
 * @param[in] name Attribute name.
 * @return 0 on success.
 */
int bud_set_bool_attr(bud_node *node, const char *name);

/**
 * @brief Read an attribute value.
 * @param[in] node Target node.
 * @param[in] name Attribute name.
 * @return Attribute value, or NULL when absent.
 */
const char *bud_get_attr(const bud_node *node, const char *name);

/**
 * @brief Add a CSS class to a node.
 * @param[in] node Target node.
 * @param[in] cls  Class name.
 * @return 0 on success.
 */
int bud_add_class(bud_node *node, const char *cls);

/**
 * @brief Remove a CSS class from a node.
 * @param[in] node Target node.
 * @param[in] cls  Class name.
 * @return 0 on success.
 */
int bud_remove_class(bud_node *node, const char *cls);

/**
 * @brief Toggle a CSS class on a node.
 * @param[in] node Target node.
 * @param[in] cls  Class name.
 * @return 0 on success.
 */
int bud_toggle_class(bud_node *node, const char *cls);

/**
 * @brief Detach a node from its parent.
 * @param[in] node Node to detach.
 * @return 0 on success.
 */
int bud_detach(bud_node *node);

/**
 * @brief Mark a node as listening for an event.
 * @param[in] node    Target node.
 * @param[in] event   Event type.
 * @param[in] bubbles Non-zero to bubble.
 * @return 0 on success.
 */
int bud_on(bud_node *node, const char *event, int bubbles);

/**
 * @brief Bind an event handler to a node.
 * @param[in] node    Target node.
 * @param[in] event   Event type.
 * @param[in] bubbles Non-zero to bubble.
 * @param[in] handler Handler callback.
 * @return 0 on success.
 */
int bud_bind(
        bud_node *node, const char *event, int bubbles,
        bud_event_handler_fn handler);

/* Universal API action dispatcher */
/**
 * @brief Dispatch a UI action encoded in the event.
 * @param[in] event Event to dispatch.
 * @return 0 on success.
 */
int bud_api_action_handler(bud_event *event);

/**
 * @brief Attach lifecycle callbacks to a node.
 * @param[in] node       Target node.
 * @param[in] on_mount   Mount callback, or NULL.
 * @param[in] on_update  Update callback, or NULL.
 * @param[in] on_unmount Unmount callback, or NULL.
 * @param[in] user       Caller data passed to each callback.
 * @return 0 on success.
 */
int bud_set_lifecycle(
        bud_node *node, bud_lifecycle_fn on_mount, bud_lifecycle_fn on_update,
        bud_lifecycle_fn on_unmount, void *user);

/**
 * @brief Append a child to a parent node.
 * @param[in] parent Parent node.
 * @param[in] child  Child node.
 * @return 0 on success.
 */
int bud_append(bud_node *parent, bud_node *child);

/**
 * @brief Kind of a node.
 * @param[in] node Target node.
 * @return Node kind.
 */
bud_node_kind bud_node_kind_of(const bud_node *node);

/**
 * @brief Tag name of an element node.
 * @param[in] node Target node.
 * @return Tag name, or NULL for non-elements.
 */
const char *bud_node_tag(const bud_node *node);

/**
 * @brief Text content of a text node.
 * @param[in] node Target node.
 * @return Text, or NULL for non-text nodes.
 */
const char *bud_node_text(const bud_node *node);

/**
 * @brief Stable id of a node.
 * @param[in] node Target node.
 * @return Node id.
 */
unsigned int bud_node_id(const bud_node *node);

/**
 * @brief Number of children of a node.
 * @param[in] node Target node.
 * @return Child count.
 */
size_t bud_node_child_count(const bud_node *node);

/**
 * @brief Child at an index.
 * @param[in] node  Target node.
 * @param[in] index Child index.
 * @return Child node, or NULL when out of range.
 */
const bud_node *bud_node_child(const bud_node *node, size_t index);

/**
 * @brief Parent of a node.
 * @param[in] node Target node.
 * @return Parent node, or NULL for roots.
 */
bud_node *bud_node_parent(const bud_node *node);

/**
 * @brief Number of attributes on a node.
 * @param[in] node Target node.
 * @return Attribute count.
 */
size_t bud_node_attr_count(const bud_node *node);

/**
 * @brief Attribute name at an index.
 * @param[in] node  Target node.
 * @param[in] index Attribute index.
 * @return Attribute name.
 */
const char *bud_node_attr_name(const bud_node *node, size_t index);

/**
 * @brief Attribute value at an index.
 * @param[in] node  Target node.
 * @param[in] index Attribute index.
 * @return Attribute value.
 */
const char *bud_node_attr_value(const bud_node *node, size_t index);

/**
 * @brief Number of listeners on a node.
 * @param[in] node Target node.
 * @return Listener count.
 */
size_t bud_node_listener_count(const bud_node *node);

/**
 * @brief Event type of a listener at an index.
 * @param[in] node  Target node.
 * @param[in] index Listener index.
 * @return Event type.
 */
const char *bud_node_listener_event(const bud_node *node, size_t index);

/**
 * @brief Bubble flag of a listener at an index.
 * @param[in] node  Target node.
 * @param[in] index Listener index.
 * @return Non-zero when the listener bubbles.
 */
int bud_node_listener_bubbles(const bud_node *node, size_t index);

/**
 * @brief Render a node tree to HTML.
 * @param[in] root Root node.
 * @return malloc'd HTML string (free with bud_free_string), or NULL.
 */
char *bud_render_html(const bud_node *root);

/**
 * @brief Render a node tree to hydration HTML with data-hook ids.
 * @param[in] root Root node.
 * @return malloc'd HTML string, or NULL.
 */
char *bud_render_hydrated_html(const bud_node *root);

/**
 * @brief Emit patch operations for a full render.
 * @param[in] root Root node.
 * @param[in] emit Emission callback.
 * @param[in] user Caller data passed to emit.
 * @return 0 on success.
 */
int bud_render_ops(const bud_node *root, bud_emit_fn emit, void *user);

/**
 * @brief Emit hydration operations for a fresh mount.
 * @param[in] root Root node.
 * @param[in] emit Emission callback.
 * @param[in] user Caller data passed to emit.
 * @return 0 on success.
 */
int bud_render_hydration_ops(
        const bud_node *root, bud_emit_fn emit, void *user);

/**
 * @brief Emit patch operations to bring a server-rendered tree up to date.
 * @param[in] root Root node.
 * @param[in] emit Emission callback.
 * @param[in] user Caller data passed to emit.
 * @return 0 on success.
 */
int bud_render_patch_ops(const bud_node *root, bud_emit_fn emit, void *user);

/**
 * @brief Diff two trees and emit minimal patch operations.
 * @param[in] old_root Previous tree.
 * @param[in] new_root New tree.
 * @param[in] emit     Emission callback.
 * @param[in] user     Caller data passed to emit.
 * @return 0 on success.
 */
int bud_vdom_diff(
        bud_node *old_root, bud_node *new_root, bud_emit_fn emit, void *user);

/**
 * @brief Signal that application state has changed (triggers re-render).
 */
void bud_app_set_state(void);

/**
 * @brief Hydrate a server-rendered tree into DOM nodes.
 * @param[in] root   Root node.
 * @param[in] lookup Hydration lookup callback.
 * @param[in] user   Caller data passed to lookup.
 * @return 0 on success.
 */
int bud_hydrate(const bud_node *root, bud_hydrate_lookup_fn lookup, void *user);

/**
 * @brief Create a runtime owning a node tree.
 * @param[in] root Root node.
 * @return New runtime.
 */
bud_runtime *bud_runtime_new(bud_node *root);

/**
 * @brief Free a runtime and its tree.
 * @param[in] runtime Runtime to free.
 */
void bud_runtime_free(bud_runtime *runtime);

/**
 * @brief Root node of a runtime.
 * @param[in] runtime Target runtime.
 * @return Root node.
 */
bud_node *bud_runtime_root(const bud_runtime *runtime);

/**
 * @brief Install an invalidation callback on a runtime.
 * @param[in] runtime Target runtime.
 * @param[in] fn      Callback, or NULL to clear.
 * @param[in] user    Caller data passed to fn.
 * @return 0 on success.
 */
int bud_runtime_set_invalidate(
        bud_runtime *runtime, bud_runtime_invalidate_fn fn, void *user);

/**
 * @brief Mark a runtime dirty.
 * @param[in] runtime Target runtime.
 * @return 0 on success.
 */
int bud_runtime_mark_dirty(bud_runtime *runtime);

/**
 * @brief Query whether a runtime is dirty.
 * @param[in] runtime Target runtime.
 * @return Non-zero when dirty.
 */
int bud_runtime_is_dirty(const bud_runtime *runtime);

/**
 * @brief Flush pending patch operations.
 * @param[in] runtime Target runtime.
 * @return 0 on success.
 */
int bud_runtime_flush(bud_runtime *runtime);

/**
 * @brief Mount the runtime root into the live document.
 * @param[in] runtime Target runtime.
 * @return 0 on success.
 */
int bud_runtime_mount(bud_runtime *runtime);

/**
 * @brief Apply a full update pass.
 * @param[in] runtime Target runtime.
 * @return 0 on success.
 */
int bud_runtime_update(bud_runtime *runtime);

/**
 * @brief Unmount the runtime root.
 * @param[in] runtime Target runtime.
 * @return 0 on success.
 */
int bud_runtime_unmount(bud_runtime *runtime);

/**
 * @brief Dispatch an event through a runtime.
 * @param[in] runtime     Target runtime.
 * @param[in] target      Receiving node.
 * @param[in] event       Event type.
 * @param[in] event_user  Caller data attached to the event.
 * @return 0 on success.
 */
int bud_runtime_dispatch(
        bud_runtime *runtime, bud_node *target, const char *event,
        void *event_user);

/**
 * @brief Stop an event from propagating further.
 * @param[in] event Event to stop.
 */
void bud_event_stop_propagation(bud_event *event);

/**
 * @brief Prevent an event's default action.
 * @param[in] event Event to cancel.
 */
void bud_event_prevent_default(bud_event *event);

/**
 * @brief Free a node tree.
 * @param[in] node Root node to free.
 */
void bud_free(bud_node *node);

/**
 * @brief Free a string returned by a bud_render_* function.
 * @param[in] value String to free.
 */
void bud_free_string(char *value);

/* JSON field extraction helpers (no json-c dependency) — jsmn-backed */
/**
 * @brief Extract a string field from NUL-terminated JSON.
 * @param[in]  json     NUL-terminated JSON.
 * @param[in]  key      Field name.
 * @param[out] out      Destination buffer.
 * @param[in]  out_size Capacity of out (NUL-terminated when > 0).
 */
void bud_json_str(
        const char *json, const char *key, char *out, size_t out_size);

/**
 * @brief Extract a string field from length-bounded JSON.
 * @param[in]  json     JSON bytes.
 * @param[in]  len      Byte length of json.
 * @param[in]  key      Field name.
 * @param[out] out      Destination buffer.
 * @param[in]  out_size Capacity of out (NUL-terminated when > 0).
 */
void bud_json_str_len(
        const char *json, size_t len, const char *key, char *out,
        size_t out_size);

/**
 * @brief Extract an integer field from NUL-terminated JSON.
 * @param[in] json        NUL-terminated JSON.
 * @param[in] key         Field name.
 * @param[in] default_val Value when the field is absent or not an integer.
 * @return Parsed integer.
 */
int bud_json_int(const char *json, const char *key, int default_val);

/**
 * @brief Extract an integer field from length-bounded JSON.
 * @param[in] json        JSON bytes.
 * @param[in] len         Byte length of json.
 * @param[in] key         Field name.
 * @param[in] default_val Value when the field is absent or not an integer.
 * @return Parsed integer.
 */
int bud_json_int_len(
        const char *json, size_t len, const char *key, int default_val);

/**
 * @brief Extract the raw "data" string field from NUL-terminated JSON.
 * @param[in]  json     NUL-terminated JSON.
 * @param[out] out      Destination buffer.
 * @param[in]  out_size Capacity of out.
 */
void bud_json_data(const char *json, char *out, size_t out_size);

/**
 * @brief Extract the raw "data" string field from length-bounded JSON.
 * @param[in]  json     JSON bytes.
 * @param[in]  len      Byte length of json.
 * @param[out] out      Destination buffer.
 * @param[in]  out_size Capacity of out.
 */
void bud_json_data_len(
        const char *json, size_t len, char *out, size_t out_size);

/**
 * @brief Iterate over a top-level JSON array (NUL-terminated).
 * @param[in] json NUL-terminated JSON array.
 * @param[in] fn   Callback per element (borrowed, length-bounded).
 * @param[in] user Caller data passed to fn.
 * @return Number of elements visited, or -1 on parse failure.
 */
int bud_json_array_for_each(
        const char *json, void (*fn)(const char *elem, size_t len, void *user),
        void *user);

/**
 * @brief Iterate over a top-level JSON array (length-bounded).
 * @param[in] json JSON array bytes.
 * @param[in] len  Byte length of json.
 * @param[in] fn   Callback per element (borrowed, length-bounded).
 * @param[in] user Caller data passed to fn.
 * @return Number of elements visited, or -1 on parse failure.
 */
int bud_json_array_for_each_len(
        const char *json, size_t len,
        void (*fn)(const char *elem, size_t len, void *user), void *user);

/**
 * @brief Iterate over the array stored under a named key.
 * @param[in] json JSON bytes.
 * @param[in] len  Byte length of json.
 * @param[in] key  Field name holding the array.
 * @param[in] fn   Callback per element (borrowed, length-bounded).
 * @param[in] user Caller data passed to fn.
 * @return Number of elements visited, or -1 on parse failure.
 */
int bud_json_array_for_each_key_len(
        const char *json, size_t len, const char *key,
        void (*fn)(const char *elem, size_t len, void *user), void *user);

/* Table-driven state: one field definition drives wasm_init, wasm_set_*
 * Pure UI binder — only key/offset/size/is_int/kind. Zero DB/storage concepts.
 */
/**
 * @brief One state-field definition driving JSON→state binding.
 */
typedef struct bud_field_desc {
	/** Field key matching the JSON name. */
	const char *key;
	/** Byte offset of the field within the state struct. */
	size_t offset;
	/** Byte size of the field. */
	size_t size;
	/** Non-zero for integer fields. */
	int is_int;
	/** Serialization kind. */
	int kind;
} bud_field_desc_t;

/**
 * @brief Apply JSON fields onto a state struct (fully flexible).
 * @param[in,out] state  Destination struct.
 * @param[in]     fields Field descriptor array, NULL-key terminated.
 * @param[in]     json   NUL-terminated JSON.
 */
void bud_state_apply(
        void *state, const bud_field_desc_t *fields, const char *json);

/**
 * @brief Apply length-bounded JSON fields onto a state struct.
 * @param[in,out] state  Destination struct.
 * @param[in]     fields Field descriptor array, NULL-key terminated.
 * @param[in]     json   JSON bytes.
 * @param[in]     len    Byte length of json.
 */
void bud_state_apply_len(
        void *state, const bud_field_desc_t *fields, const char *json,
        size_t len);

/**
 * @brief Apply JSON onto a state struct with caller-specified field stride.
 * @param[in,out] state        Destination struct.
 * @param[in]     fields       Field descriptors.
 * @param[in]     field_stride Stride between descriptors.
 * @param[in]     json         NUL-terminated JSON.
 */
void bud_state_apply_stride(
        void *state, const void *fields, size_t field_stride, const char *json);

/**
 * @brief Apply length-bounded JSON with caller-specified field stride.
 * @param[in,out] state        Destination struct.
 * @param[in]     fields       Field descriptors.
 * @param[in]     field_stride Stride between descriptors.
 * @param[in]     json         JSON bytes.
 * @param[in]     len          Byte length of json.
 */
void bud_state_apply_stride_len(
        void *state, const void *fields, size_t field_stride, const char *json,
        size_t len);

/**
 * @brief Populate a fixed-capacity array from a JSON array field.
 * @param[in]     json       NUL-terminated JSON.
 * @param[in]     key        Field name holding the array.
 * @param[out]    array_out  Destination array.
 * @param[in]     elem_size  Byte size of one element.
 * @param[in,out] count_out  In: capacity hint; out: filled count.
 * @param[in]     max_elems  Hard element cap.
 * @param[in]     schema     Per-field descriptors for each element.
 */
void bud_state_apply_array(
        const char *json, const char *key, void *array_out, size_t elem_size,
        int *count_out, int max_elems, const bud_field_desc_t *schema);

/**
 * @brief Populate a fixed-capacity array from a length-bounded JSON array.
 * @param[in]     json       JSON bytes.
 * @param[in]     len        Byte length of json.
 * @param[in]     key        Field name holding the array.
 * @param[out]    array_out  Destination array.
 * @param[in]     elem_size  Byte size of one element.
 * @param[in,out] count_out  In: capacity hint; out: filled count.
 * @param[in]     max_elems  Hard element cap.
 * @param[in]     schema     Per-field descriptors for each element.
 */
void bud_state_apply_array_len(
        const char *json, size_t len, const char *key, void *array_out,
        size_t elem_size, int *count_out, int max_elems,
        const bud_field_desc_t *schema);

/**
 * @brief Populate a fixed-capacity array with caller-specified schema stride.
 * @param[in]     json          JSON bytes.
 * @param[in]     len           Byte length of json.
 * @param[in]     key           Field name holding the array.
 * @param[out]    array_out     Destination array.
 * @param[in]     elem_size     Byte size of one element.
 * @param[in,out] count_out     In: capacity hint; out: filled count.
 * @param[in]     max_elems     Hard element cap.
 * @param[in]     schema        Per-field descriptors for each element.
 * @param[in]     schema_stride Stride between element descriptors.
 */
void bud_state_apply_array_stride_len(
        const char *json, size_t len, const char *key, void *array_out,
        size_t elem_size, int *count_out, int max_elems, const void *schema,
        size_t schema_stride);

/* Debug helpers — available in all builds, most useful with BUD_DEBUG */
/**
 * @brief Stamp a debug source location onto a node.
 * @param[in] node Target node.
 * @param[in] file Source file name.
 * @param[in] line Source line number.
 */
void bud_node_set_src(bud_node *node, const char *file, int line);

/**
 * @brief Read a node's stamped source file.
 * @param[in] node Target node.
 * @return Source file name, or NULL.
 */
const char *bud_node_get_src(const bud_node *node);

/**
 * @brief Render a compact ASCII tree dump for debugging.
 * @param[in]  root  Root node.
 * @param[out] buf   Destination buffer.
 * @param[in]  bufsz Capacity of buf.
 * @return Bytes written, or -1 when truncated.
 */
int bud_sprint_tree(const bud_node *root, char *buf, size_t bufsz);

/* Platform host function pointers (set by WASM adapter, NULL on native) */
extern void (*bud_host_fetch_fn)(const char *url, size_t len, int id);
extern void (*bud_host_log_fn)(const char *msg, size_t len);
extern void (*bud_host_set_location_fn)(const char *url, size_t len);

#ifdef __cplusplus
}
#endif

#endif
