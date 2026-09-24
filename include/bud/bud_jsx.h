#ifndef BUD_BUD_JSX_H
#define BUD_BUD_JSX_H

/**
 * @file bud_jsx.h
 * @brief JSX-style builder macros for bud node trees.
 *
 * Compiles attribute lists, bindings and nested nodes into bud_node
 * construction calls; used by both SSR and WASM builds.
 */

#include <stdio.h>

#include "bud.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Discriminator for a builder argument.
 */
typedef enum bud_arg_type {
	/** No-op placeholder. */
	BUD_ARG_NONE = 0,
	/** Child node. */
	BUD_ARG_NODE = 1,
	/** Attribute name/value pair. */
	BUD_ARG_ATTR = 2,
	/** Event listener registration. */
	BUD_ARG_EVENT = 3,
	/** Event handler binding. */
	BUD_ARG_BIND = 4,
	/** Formatting attribute (name + printf format). */
	BUD_ARG_ATTR_FMT = 5
} bud_arg_type;

/**
 * @brief One builder argument consumed by lx_el/lx_frag.
 */
typedef struct bud_arg {
	/** Argument kind. */
	bud_arg_type type;
	/** Kind-specific payload. */
	union {
		/** Child node (BUD_ARG_NODE). */
		bud_node *node;
		/** Attribute (BUD_ARG_ATTR). */
		struct {
			const char *name;
			const char *value;
		} attr;
		/** Event registration (BUD_ARG_EVENT). */
		struct {
			const char *event;
			int bubbles;
		} ev;
		/** Handler binding (BUD_ARG_BIND). */
		struct {
			const char *event;
			int bubbles;
			bud_event_handler_fn handler;
		} bind;
	} data;
} bud_arg;

/** @brief No-op builder argument. */
#define lx_none() ((bud_arg){ .type = BUD_ARG_NONE })
/** @brief Selector helper for the lx_attr() arity dispatch. */
#define _bud_ATTR_SEL(_1, _2, _3, _4, _5, _6, _7, NAME, ...) NAME
/** @brief Build an attribute argument; extra args become a printf format. */
#define lx_attr(...)                                                           \
	_bud_ATTR_SEL(                                                         \
	        __VA_ARGS__, _bud_attr_7, _bud_attr_6, _bud_attr_5,            \
	        _bud_attr_4, _bud_attr_3, _bud_attr_2)(__VA_ARGS__)

/** @brief Two-arg form: literal name/value attribute. */
#define _bud_attr_2(k, v)                                                      \
	((bud_arg){ .type = BUD_ARG_ATTR, .data.attr = { (k), (v) } })
/** @brief N-arg form dispatch for formatted attributes. */
#define _bud_attr_3(k, fmt, ...) _bud_attr_N(k, fmt, ##__VA_ARGS__)
#define _bud_attr_4(k, fmt, ...) _bud_attr_N(k, fmt, ##__VA_ARGS__)
#define _bud_attr_5(k, fmt, ...) _bud_attr_N(k, fmt, ##__VA_ARGS__)
#define _bud_attr_6(k, fmt, ...) _bud_attr_N(k, fmt, ##__VA_ARGS__)
#define _bud_attr_7(k, fmt, ...) _bud_attr_N(k, fmt, ##__VA_ARGS__)

/** @brief Formatted attribute implementation. */
#define _bud_attr_N(k, fmt, ...) bud_attr_fmt(k, fmt, ##__VA_ARGS__)
/** @brief Text child argument. */
#define lx_text(str)                                                           \
	((bud_arg){ .type = BUD_ARG_NODE, .data.node = bud_text(str) })
/** @brief Foreign node child argument. */
#define lx_node(n) ((bud_arg){ .type = BUD_ARG_NODE, .data.node = (n) })
/** @brief Raw-HTMl child argument from a printf format. */
#define lx_tpl(...)                                                            \
	((bud_arg){ .type = BUD_ARG_NODE, .data.node = bud_tpl(__VA_ARGS__) })
/** @brief Event registration argument. */
#define lx_on(e, b)                                                            \
	((bud_arg){ .type = BUD_ARG_EVENT, .data.ev = { (e), (b) } })
/** @brief Handler binding argument. */
#define lx_bind(e, b, h)                                                       \
	((bud_arg){ .type = BUD_ARG_BIND, .data.bind = { (e), (b), (h) } })

/**
 * @brief Assemble an element from builder arguments.
 * @param[in] tag   Element tag.
 * @param[in] count Number of args.
 * @param[in] args  Builder argument array.
 * @return New element node.
 */
bud_node *bud_el_impl(const char *tag, size_t count, const bud_arg *args);

/**
 * @brief Assemble a fragment from builder arguments.
 * @param[in] count Number of args.
 * @param[in] args  Builder argument array.
 * @return New fragment node.
 */
bud_node *bud_frag_impl(size_t count, const bud_arg *args);

/**
 * @brief Build a formatted attribute argument.
 * @param[in] name Attribute name.
 * @param[in] fmt  Format string.
 * @return Attribute argument whose value is formatted lazily at assembly.
 */
bud_arg bud_attr_fmt(const char *name, const char *fmt, ...);

/* Note: In C99, __VA_ARGS__ cannot be empty. For empty nodes, use lx_el("tag",
 * lx_none()) */
/** @brief Element child argument from a tag and builder args. */
#define lx_el(tag, ...)                                                        \
	((bud_arg){                                                            \
	        .type = BUD_ARG_NODE,                                          \
	        .data.node = bud_el_impl(                                      \
	                (tag),                                                 \
	                sizeof((bud_arg[]){ __VA_ARGS__ }) / sizeof(bud_arg),  \
	                (bud_arg[]){ __VA_ARGS__ }) })

/** @brief Fragment child argument from builder args. */
#define lx_frag(...)                                                           \
	((bud_arg){                                                            \
	        .type = BUD_ARG_NODE,                                          \
	        .data.node = bud_frag_impl(                                    \
	                sizeof((bud_arg[]){ __VA_ARGS__ }) / sizeof(bud_arg),  \
	                (bud_arg[]){ __VA_ARGS__ }) })

/** @brief Standalone element builder (statement form). */
#define lx_n(tag, ...)                                                         \
	bud_el_impl((tag),                                                     \
	            sizeof((bud_arg[]){ __VA_ARGS__ }) / sizeof(bud_arg),      \
	            (bud_arg[]){ __VA_ARGS__ })

/** @brief Standalone fragment builder (statement form). */
#define lx_frag_n(...)                                                         \
	bud_frag_impl(sizeof((bud_arg[]){ __VA_ARGS__ }) / sizeof(bud_arg),    \
	              (bud_arg[]){ __VA_ARGS__ })

/** @brief Formatted text child argument. */
#define lx_textf(fmt, ...)                                                     \
	((bud_arg){ .type = BUD_ARG_NODE,                                      \
	            .data.node = bud_textf((fmt), ##__VA_ARGS__) })

/** @brief Hidden-input child argument. */
#define lx_hidden(name, val)                                                   \
	((bud_arg){ .type = BUD_ARG_NODE,                                      \
	            .data.node = bud_hidden_input((name), (val)) })

/** @brief Integer hidden-input child argument. */
#define lx_hidden_int(name, val)                                               \
	((bud_arg){ .type = BUD_ARG_NODE,                                      \
	            .data.node = bud_hidden_input_int((name), (val)) })

/** @brief Submit-button child argument. */
#define lx_submit(label, cls)                                                  \
	((bud_arg){ .type = BUD_ARG_NODE,                                      \
	            .data.node = bud_submit_btn((label), (cls)) })

/** @brief Link child argument. */
#define lx_link(href, text, cls)                                               \
	((bud_arg){ .type = BUD_ARG_NODE,                                      \
	            .data.node = bud_link((href), (text), (cls)) })

/* ── BUD_DEBUG: source location tracking ── */
/* When BUD_DEBUG is defined, lx_el/lx_text/lx_frag capture __FILE__/__LINE__
 * and stamp it on the created node.  lx_raw() is also provided as a
 * source-aware wrapper for bud_raw().  Use bud_sprint_tree() in native
 * builds and wasm_dump_tree() from WASM to print a tree with source info.
 */
#ifdef BUD_DEBUG
/**
 * @brief Stamp a source location on a node and wrap it in a builder arg.
 * @param[in] node Node to stamp.
 * @param[in] file Source file.
 * @param[in] line Source line.
 * @return Builder argument wrapping node.
 */
static inline bud_arg _bud_src_node(bud_node *node, const char *file, int line)
{
	bud_node_set_src(node, file, line);
	return (bud_arg){ .type = BUD_ARG_NODE, .data.node = node };
}

#undef lx_text
#define lx_text(str) _bud_src_node(bud_text(str), __FILE__, __LINE__)

#undef lx_el
#define lx_el(tag, ...)                                                        \
	_bud_src_node(                                                         \
	        bud_el_impl(                                                   \
	                (tag),                                                 \
	                sizeof((bud_arg[]){ __VA_ARGS__ }) / sizeof(bud_arg),  \
	                (bud_arg[]){ __VA_ARGS__ }),                           \
	        __FILE__, __LINE__)

#undef lx_frag
#define lx_frag(...)                                                           \
	_bud_src_node(                                                         \
	        bud_frag_impl(                                                 \
	                sizeof((bud_arg[]){ __VA_ARGS__ }) / sizeof(bud_arg),  \
	                (bud_arg[]){ __VA_ARGS__ }),                           \
	        __FILE__, __LINE__)

/**
 * @brief Stamp a source location on a node and return it directly.
 * @param[in] node Node to stamp.
 * @param[in] file Source file.
 * @param[in] line Source line.
 * @return node.
 */
static inline bud_node *_bud_src_node_ptr(bud_node *node, const char *file, int line)
{
	bud_node_set_src(node, file, line);
	return node;
}

#undef lx_n
#define lx_n(tag, ...)                                                         \
	_bud_src_node_ptr(                                                     \
	        bud_el_impl((tag),                                             \
	                    sizeof((bud_arg[]){ __VA_ARGS__ }) / sizeof(bud_arg), \
	                    (bud_arg[]){ __VA_ARGS__ }),                       \
	        __FILE__, __LINE__)

#undef lx_frag_n
#define lx_frag_n(...)                                                         \
	_bud_src_node_ptr(                                                     \
	        bud_frag_impl(sizeof((bud_arg[]){ __VA_ARGS__ }) / sizeof(bud_arg), \
	                      (bud_arg[]){ __VA_ARGS__ }),                     \
	        __FILE__, __LINE__)

#undef lx_textf
#define lx_textf(fmt, ...)                                                     \
	_bud_src_node(bud_textf((fmt), ##__VA_ARGS__), __FILE__, __LINE__)

#undef lx_hidden
#define lx_hidden(name, val)                                                   \
	_bud_src_node(bud_hidden_input((name), (val)), __FILE__, __LINE__)

#undef lx_hidden_int
#define lx_hidden_int(name, val)                                               \
	_bud_src_node(bud_hidden_input_int((name), (val)), __FILE__, __LINE__)

#undef lx_submit
#define lx_submit(label, cls)                                                  \
	_bud_src_node(bud_submit_btn((label), (cls)), __FILE__, __LINE__)

#undef lx_link
#define lx_link(href, text, cls)                                               \
	_bud_src_node(bud_link((href), (text), (cls)), __FILE__, __LINE__)

/* lx_raw wraps bud_raw with source tracking (use as a child arg to lx_el) */
#define lx_raw(html) _bud_src_node(bud_raw(html), __FILE__, __LINE__)
#endif /* BUD_DEBUG */

#ifdef __cplusplus
}
#endif

#endif
