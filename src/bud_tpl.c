#include "bud_internal.h"

static int is_html_void_tag(const char *tag)
{
	static const char *void_tags[] = { "area",  "base", "br",    "col",
		                           "embed", "hr",   "img",   "input",
		                           "link",  "meta", "param", "source",
		                           "track", "wbr",  NULL };
	if (!tag)
		return 0;
	for (int i = 0; void_tags[i]; i++) {
		if (strcmp(tag, void_tags[i]) == 0)
			return 1;
	}
	return 0;
}

static void bud_tpl_flush_text(bud_node *parent, char *buf, size_t *len)
{
	if (*len == 0 || !parent)
		return;
	buf[*len] = '\0';
	int all_ws = 1;
	for (size_t i = 0; i < *len; i++) {
		if (buf[i] != ' ' && buf[i] != '\t' && buf[i] != '\n' &&
		    buf[i] != '\r')
		{
			all_ws = 0;
			break;
		}
	}
	if (!all_ws) {
		bud_node *txt = bud_text(buf);
		if (txt)
			bud_append(parent, txt);
	}
	*len = 0;
}

bud_node *bud_vtpl(const char *fmt, va_list ap)
{
	if (!fmt)
		return NULL;

	bud_node *stack[64];
	int depth = 0;

	bud_node *root = bud_fragment();
	if (!root)
		return NULL;
	stack[depth++] = root;

	char text_buf[4096];
	size_t text_len = 0;

	const char *p = fmt;

	while (*p) {
		if (*p == '<') {
			/* If comment: <!-- ... --> */
			if (p[1] == '!' && p[2] == '-' && p[3] == '-') {
				bud_tpl_flush_text(
				        stack[depth - 1], text_buf, &text_len);
				p += 4;
				while (*p && !(*p == '-' && p[1] == '-' &&
				               p[2] == '>'))
				{
					p++;
				}
				if (*p)
					p += 3;
				continue;
			}

			/* If closing tag: </tag> */
			if (p[1] == '/') {
				bud_tpl_flush_text(
				        stack[depth - 1], text_buf, &text_len);
				p += 2;
				const char *tag_start = p;
				while (*p && *p != '>' && *p != ' ' &&
				       *p != '\t' && *p != '\n' && *p != '\r')
				{
					p++;
				}
				char tag_name[64];
				size_t tlen = (size_t)(p - tag_start);
				if (tlen >= sizeof(tag_name))
					tlen = sizeof(tag_name) - 1;
				memcpy(tag_name, tag_start, tlen);
				tag_name[tlen] = '\0';

				while (*p && *p != '>')
					p++;
				if (*p == '>')
					p++;

				if (depth > 1) {
					for (int d = depth - 1; d >= 1; d--) {
						const char *st_tag =
						        bud_node_tag(stack[d]);
						if (st_tag &&
						    strcmp(st_tag, tag_name) ==
						            0)
						{
							depth = d;
							break;
						}
					}
				}
				continue;
			}

			/* Opening tag: <tag ... > */
			if ((p[1] >= 'a' && p[1] <= 'z') ||
			    (p[1] >= 'A' && p[1] <= 'Z'))
			{
				bud_tpl_flush_text(
				        stack[depth - 1], text_buf, &text_len);
				p++; /* skip '<' */
				const char *tag_start = p;
				while (*p && *p != '>' && *p != '/' &&
				       *p != ' ' && *p != '\t' && *p != '\n' &&
				       *p != '\r')
				{
					p++;
				}
				char tag_name[64];
				size_t tlen = (size_t)(p - tag_start);
				if (tlen >= sizeof(tag_name))
					tlen = sizeof(tag_name) - 1;
				memcpy(tag_name, tag_start, tlen);
				tag_name[tlen] = '\0';

				bud_node *elem = bud_element(tag_name);
				if (!elem)
					break;

				bud_append(stack[depth - 1], elem);

				int self_closing = is_html_void_tag(tag_name);

				/* Parse attributes */
				while (*p && *p != '>' && *p != '/') {
					while (*p == ' ' || *p == '\t' ||
					       *p == '\n' || *p == '\r')
						p++;
					if (!*p || *p == '>' || *p == '/')
						break;

					/* Check for %bind */
					if (strncmp(p, "%bind", 5) == 0) {
						p += 5;
						const char *ev_name = va_arg(
						        ap, const char *);
						bud_event_handler_fn handler = va_arg(
						        ap,
						        bud_event_handler_fn);
						if (ev_name && handler) {
							bud_bind(
							        elem, ev_name,
							        0, handler);
						}
						continue;
					}

					/* Check for %b (boolean attribute e.g.
					 * "selected", "checked", "disabled", or
					 * NULL) */
					if (strncmp(p, "%b", 2) == 0 &&
					    (p[2] == ' ' || p[2] == '\t' ||
					     p[2] == '\n' || p[2] == '\r' ||
					     p[2] == '>' || p[2] == '/'))
					{
						p += 2;
						const char *b_name = va_arg(
						        ap, const char *);
						if (b_name && b_name[0]) {
							bud_set_bool_attr(
							        elem, b_name);
						}
						continue;
					}

					/* Parse attribute name */
					const char *attr_start = p;
					while (*p && *p != '=' && *p != '>' &&
					       *p != '/' && *p != ' ' &&
					       *p != '\t' && *p != '\n' &&
					       *p != '\r')
					{
						p++;
					}
					char attr_name[64];
					size_t alen = (size_t)(p - attr_start);
					if (alen >= sizeof(attr_name))
						alen = sizeof(attr_name) - 1;
					memcpy(attr_name, attr_start, alen);
					attr_name[alen] = '\0';

					while (*p == ' ' || *p == '\t' ||
					       *p == '\n' || *p == '\r')
						p++;

					if (*p == '=') {
						p++;
						while (*p == ' ' ||
						       *p == '\t' ||
						       *p == '\n' || *p == '\r')
							p++;
						char attr_val[2048];
						size_t vlen = 0;

						if (*p == '"' || *p == '\'') {
							char quote = *p++;
							while (*p &&
							       *p != quote)
							{
								if (*p == '%') {
									if (p[1] ==
									    's')
									{
										const char *s = va_arg(
										        ap,
										        const char
										                *);
										if (s)
										{
											size_t sl = strlen(
											        s);
											if (vlen + sl <
											    sizeof(attr_val))
											{
												memcpy(attr_val +
												               vlen,
												       s,
												       sl);
												vlen += sl;
											}
										}
										p += 2;
										continue;
									} else if (
									        p[1] == 'd' ||
									        p[1] == 'i')
									{
										int d = va_arg(
										        ap,
										        int);
										char num[32];
										int nl = snprintf(
										        num,
										        sizeof(num),
										        "%d",
										        d);
										if (nl > 0 &&
										    vlen + (size_t)nl <
										            sizeof(attr_val))
										{
											memcpy(attr_val +
											               vlen,
											       num,
											       (size_t)nl);
											vlen += (size_t)
											        nl;
										}
										p += 2;
										continue;
									} else if (
									        strncmp(p,
									                "%zu",
									                3) ==
									        0)
									{
										size_t z = va_arg(
										        ap,
										        size_t);
										char num[32];
										int nl = snprintf(
										        num,
										        sizeof(num),
										        "%zu",
										        z);
										if (nl > 0 &&
										    vlen + (size_t)nl <
										            sizeof(attr_val))
										{
											memcpy(attr_val +
											               vlen,
											       num,
											       (size_t)nl);
											vlen += (size_t)
											        nl;
										}
										p += 3;
										continue;
									} else if (
									        strncmp(p,
									                "%lu",
									                3) ==
									        0)
									{
										unsigned long lu = va_arg(
										        ap,
										        unsigned long);
										char num[32];
										int nl = snprintf(
										        num,
										        sizeof(num),
										        "%lu",
										        lu);
										if (nl > 0 &&
										    vlen + (size_t)nl <
										            sizeof(attr_val))
										{
											memcpy(attr_val +
											               vlen,
											       num,
											       (size_t)nl);
											vlen += (size_t)
											        nl;
										}
										p += 3;
										continue;
									} else if (
									        strncmp(p,
									                "%ld",
									                3) ==
									        0)
									{
										long ld = va_arg(
										        ap,
										        long);
										char num[32];
										int nl = snprintf(
										        num,
										        sizeof(num),
										        "%ld",
										        ld);
										if (nl > 0 &&
										    vlen + (size_t)nl <
										            sizeof(attr_val))
										{
											memcpy(attr_val +
											               vlen,
											       num,
											       (size_t)nl);
											vlen += (size_t)
											        nl;
										}
										p += 3;
										continue;
									} else if (
									        p[1] ==
									        'u')
									{
										unsigned int u = va_arg(
										        ap,
										        unsigned int);
										char num[32];
										int nl = snprintf(
										        num,
										        sizeof(num),
										        "%u",
										        u);
										if (nl > 0 &&
										    vlen + (size_t)nl <
										            sizeof(attr_val))
										{
											memcpy(attr_val +
											               vlen,
											       num,
											       (size_t)nl);
											vlen += (size_t)
											        nl;
										}
										p += 2;
										continue;
									} else if (
									        p[1] ==
									        '%')
									{
										if (vlen + 1 <
										    sizeof(attr_val))
											attr_val[vlen++] =
											        '%';
										p += 2;
										continue;
									}
								}
								if (vlen + 1 <
								    sizeof(attr_val))
								{
									attr_val[vlen++] =
									        *p;
								}
								p++;
							}
							if (*p == quote)
								p++;
						} else {
							while (*p &&
							       *p != ' ' &&
							       *p != '\t' &&
							       *p != '\n' &&
							       *p != '\r' &&
							       *p != '>' &&
							       *p != '/')
							{
								if (*p == '%' &&
								    p[1] == 's')
								{
									const char *s = va_arg(
									        ap,
									        const char
									                *);
									if (s) {
										size_t sl = strlen(
										        s);
										if (vlen + sl <
										    sizeof(attr_val))
										{
											memcpy(attr_val +
											               vlen,
											       s,
											       sl);
											vlen += sl;
										}
									}
									p += 2;
									continue;
								}
								if (vlen + 1 <
								    sizeof(attr_val))
								{
									attr_val[vlen++] =
									        *p;
								}
								p++;
							}
						}
						attr_val[vlen] = '\0';
						bud_set_attr(
						        elem, attr_name,
						        attr_val);
					} else {
						bud_set_bool_attr(
						        elem, attr_name);
					}
				}

				while (*p == ' ' || *p == '\t' || *p == '\n' ||
				       *p == '\r')
					p++;
				if (*p == '/' && p[1] == '>') {
					self_closing = 1;
					p += 2;
				} else if (*p == '>') {
					p++;
				}

				if (!self_closing && depth < 63) {
					stack[depth++] = elem;
				}
				continue;
			}
		}

		/* Check for body specifiers: %s, %d, %i, %u, %node, %raw, %% */
		if (*p == '%') {
			if (strncmp(p, "%node", 5) == 0) {
				bud_tpl_flush_text(
				        stack[depth - 1], text_buf, &text_len);
				p += 5;
				bud_node *child = va_arg(ap, bud_node *);
				if (child) {
					bud_append(stack[depth - 1], child);
				}
				continue;
			} else if (strncmp(p, "%raw", 4) == 0) {
				bud_tpl_flush_text(
				        stack[depth - 1], text_buf, &text_len);
				p += 4;
				const char *raw_html = va_arg(ap, const char *);
				if (raw_html) {
					bud_node *rnode = bud_raw(raw_html);
					if (rnode)
						bud_append(
						        stack[depth - 1],
						        rnode);
				}
				continue;
			} else if (p[1] == 's') {
				bud_tpl_flush_text(
				        stack[depth - 1], text_buf, &text_len);
				p += 2;
				const char *str = va_arg(ap, const char *);
				if (str && str[0]) {
					bud_node *tn = bud_text(str);
					if (tn)
						bud_append(
						        stack[depth - 1], tn);
				}
				continue;
			} else if (p[1] == 'd' || p[1] == 'i') {
				bud_tpl_flush_text(
				        stack[depth - 1], text_buf, &text_len);
				p += 2;
				int val = va_arg(ap, int);
				char num[32];
				snprintf(num, sizeof(num), "%d", val);
				bud_node *tn = bud_text(num);
				if (tn)
					bud_append(stack[depth - 1], tn);
				continue;
			} else if (strncmp(p, "%zu", 3) == 0) {
				bud_tpl_flush_text(
				        stack[depth - 1], text_buf, &text_len);
				p += 3;
				size_t val = va_arg(ap, size_t);
				char num[32];
				snprintf(num, sizeof(num), "%zu", val);
				bud_node *tn = bud_text(num);
				if (tn)
					bud_append(stack[depth - 1], tn);
				continue;
			} else if (strncmp(p, "%lu", 3) == 0) {
				bud_tpl_flush_text(
				        stack[depth - 1], text_buf, &text_len);
				p += 3;
				unsigned long val = va_arg(ap, unsigned long);
				char num[32];
				snprintf(num, sizeof(num), "%lu", val);
				bud_node *tn = bud_text(num);
				if (tn)
					bud_append(stack[depth - 1], tn);
				continue;
			} else if (strncmp(p, "%ld", 3) == 0) {
				bud_tpl_flush_text(
				        stack[depth - 1], text_buf, &text_len);
				p += 3;
				long val = va_arg(ap, long);
				char num[32];
				snprintf(num, sizeof(num), "%ld", val);
				bud_node *tn = bud_text(num);
				if (tn)
					bud_append(stack[depth - 1], tn);
				continue;
			} else if (p[1] == 'u') {
				bud_tpl_flush_text(
				        stack[depth - 1], text_buf, &text_len);
				p += 2;
				unsigned int val = va_arg(ap, unsigned int);
				char num[32];
				snprintf(num, sizeof(num), "%u", val);
				bud_node *tn = bud_text(num);
				if (tn)
					bud_append(stack[depth - 1], tn);
				continue;
			} else if (p[1] == '%') {
				if (text_len + 1 < sizeof(text_buf)) {
					text_buf[text_len++] = '%';
				}
				p += 2;
				continue;
			}
		}

		if (text_len + 1 < sizeof(text_buf)) {
			text_buf[text_len++] = *p;
		}
		p++;
	}

	bud_tpl_flush_text(stack[depth - 1], text_buf, &text_len);

	if (bud_node_child_count(root) == 1) {
		bud_node *first = (bud_node *)bud_node_child(root, 0);
		if (first && bud_node_kind_of(first) == BUD_NODE_ELEMENT) {
			bud_detach(first);
			bud_free(root);
			return first;
		}
	}

	return root;
}

bud_node *bud_tpl(const char *fmt, ...)
{
	va_list ap;
	bud_node *result;

	va_start(ap, fmt);
	result = bud_vtpl(fmt, ap);
	va_end(ap);

	return result;
}

bud_node *bud_el_impl(const char *tag, size_t count, const bud_arg *args)
{
	bud_node *node;
	size_t i;

	node = bud_element(tag);
	if (!node) {
		return NULL;
	}

	for (i = 0; i < count; i++) {
		switch (args[i].type) {
		case BUD_ARG_NODE:
			if (args[i].data.node) {
				bud_append(node, args[i].data.node);
			}
			break;
		case BUD_ARG_ATTR:
			bud_set_attr(
			        node, args[i].data.attr.name,
			        args[i].data.attr.value);
			break;
		case BUD_ARG_ATTR_FMT:
			bud_set_attr(
			        node, args[i].data.attr.name,
			        args[i].data.attr.value);
			free((void *)args[i].data.attr.value);
			break;
		case BUD_ARG_EVENT:
			bud_on(node, args[i].data.ev.event,
			       args[i].data.ev.bubbles);
			break;
		case BUD_ARG_BIND:
			bud_bind(
			        node, args[i].data.bind.event,
			        args[i].data.bind.bubbles,
			        args[i].data.bind.handler);
			break;
		case BUD_ARG_NONE:
		default:
			break;
		}
	}

	return node;
}

bud_node *bud_frag_impl(size_t count, const bud_arg *args)
{
	bud_node *node;
	size_t i;

	node = bud_fragment();
	if (!node) {
		return NULL;
	}

	for (i = 0; i < count; i++) {
		switch (args[i].type) {
		case BUD_ARG_NODE:
			if (args[i].data.node) {
				bud_append(node, args[i].data.node);
			}
			break;
		case BUD_ARG_NONE:
		case BUD_ARG_ATTR:
		case BUD_ARG_EVENT:
		case BUD_ARG_BIND:
		default:
			break;
		}
	}

	return node;
}
