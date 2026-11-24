/**
 * @file cx_lsp_test.c
 * @brief End-to-end LSP JSON-RPC protocol tests for `bin/cx-lsp` (both standalone
 *        JSX mode and live `clangd` proxy mode).
 */

#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
	int    to_srv;
	int    from_srv;
	pid_t  pid;
	char  *buf;
	size_t len;
	size_t cap;
} lsp_client;

static int write_full(int fd, const char *s, size_t n)
{
	size_t off = 0;
	while (off < n) {
		ssize_t w = write(fd, s + off, n - off);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		off += (size_t)w;
	}
	return 0;
}

static void lsp_send(lsp_client *c, const char *json)
{
	size_t n = strlen(json);
	char hdr[64];
	int hn = snprintf(hdr, sizeof(hdr), "Content-Length: %zu\r\n\r\n", n);
	assert(write_full(c->to_srv, hdr, (size_t)hn) == 0);
	assert(write_full(c->to_srv, json, n) == 0);
}

static char *lsp_recv(lsp_client *c, int timeout_ms)
{
	for (;;) {
		/* Check if buffer already contains a full message */
		size_t hdr_end = 0;
		int found = 0;
		for (size_t i = 0; i + 3 < c->len; i++) {
			if (c->buf[i] == '\r' && c->buf[i + 1] == '\n' &&
			    c->buf[i + 2] == '\r' && c->buf[i + 3] == '\n')
			{
				hdr_end = i + 4;
				found = 1;
				break;
			}
		}
		if (found) {
			const char *cl = strstr(c->buf, "Content-Length:");
			assert(cl != NULL && cl < c->buf + hdr_end);
			long blen = strtol(cl + 15, NULL, 10);
			assert(blen >= 0);
			size_t total = hdr_end + (size_t)blen;
			if (c->len >= total) {
				char *body = (char *)malloc((size_t)blen + 1);
				assert(body != NULL);
				memcpy(body, c->buf + hdr_end, (size_t)blen);
				body[blen] = '\0';
				size_t rem = c->len - total;
				if (rem > 0)
					memmove(c->buf, c->buf + total, rem);
				c->len = rem;
				return body;
			}
		}

		struct pollfd pfd = { .fd = c->from_srv, .events = POLLIN, .revents = 0 };
		int pr = poll(&pfd, 1, timeout_ms);
		if (pr <= 0)
			return NULL;

		if (c->len + 4096 > c->cap) {
			size_t ncap = c->cap ? c->cap * 2 : 8192;
			c->buf = (char *)realloc(c->buf, ncap);
			assert(c->buf != NULL);
			c->cap = ncap;
		}
		ssize_t r = read(c->from_srv, c->buf + c->len, c->cap - c->len);
		if (r <= 0)
			return NULL;
		c->len += (size_t)r;
	}
}

static char *lsp_wait_id(lsp_client *c, int id, int timeout_ms)
{
	char needle1[32], needle2[32];
	snprintf(needle1, sizeof(needle1), "\"id\":%d,", id);
	snprintf(needle2, sizeof(needle2), "\"id\":%d}", id);
	for (int iter = 0; iter < 64; iter++) {
		char *msg = lsp_recv(c, timeout_ms);
		assert(msg != NULL);
		if (strstr(msg, needle1) || strstr(msg, needle2))
			return msg;
		free(msg);
	}
	assert(!"timed out waiting for response id");
	return NULL;
}

static char *lsp_wait_method(lsp_client *c, const char *method, int timeout_ms)
{
	char needle[128];
	snprintf(needle, sizeof(needle), "\"method\":\"%s\"", method);
	for (int iter = 0; iter < 64; iter++) {
		char *msg = lsp_recv(c, timeout_ms);
		assert(msg != NULL);
		if (strstr(msg, needle))
			return msg;
		free(msg);
	}
	assert(!"timed out waiting for notification method");
	return NULL;
}

static void lsp_start(lsp_client *c, const char *bin, char *const *argv)
{
	memset(c, 0, sizeof(*c));
	int p_in[2], p_out[2];
	assert(pipe(p_in) == 0);
	assert(pipe(p_out) == 0);

	pid_t pid = fork();
	assert(pid >= 0);
	if (pid == 0) {
		dup2(p_in[0], STDIN_FILENO);
		dup2(p_out[1], STDOUT_FILENO);
		close(p_in[0]);
		close(p_in[1]);
		close(p_out[0]);
		close(p_out[1]);
		execv(bin, argv);
		_exit(127);
	}
	close(p_in[0]);
	close(p_out[1]);
	c->to_srv = p_in[1];
	c->from_srv = p_out[0];
	c->pid = pid;
}

static void lsp_stop(lsp_client *c)
{
	lsp_send(c, "{\"jsonrpc\":\"2.0\",\"id\":999,\"method\":\"shutdown\",\"params\":null}");
	char *r = lsp_wait_id(c, 999, 5000);
	free(r);
	lsp_send(c, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\",\"params\":null}");
	close(c->to_srv);
	close(c->from_srv);
	int status = 0;
	waitpid(c->pid, &status, 0);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	free(c->buf);
}

static char *json_escape_str(const char *s)
{
	size_t n = strlen(s);
	char *out = (char *)malloc(n * 2 + 1);
	assert(out != NULL);
	size_t k = 0;
	for (size_t i = 0; i < n; i++) {
		if (s[i] == '"') { out[k++] = '\\'; out[k++] = '"'; }
		else if (s[i] == '\\') { out[k++] = '\\'; out[k++] = '\\'; }
		else if (s[i] == '\n') { out[k++] = '\\'; out[k++] = 'n'; }
		else out[k++] = s[i];
	}
	out[k] = '\0';
	return out;
}

/* ── Test 1: Direct JSX Tag/Component/Prop Completions & CX Diagnostics ─ */

static void test_jsx_completions_and_cx_diags(const char *lsp_bin)
{
	char *argv[] = { (char *)lsp_bin, (char *)"--no-clangd", NULL };
	lsp_client c;
	lsp_start(&c, lsp_bin, argv);

	lsp_send(&c, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":null,\"capabilities\":{}}}");
	char *init_r = lsp_wait_id(&c, 1, 5000);
	assert(strstr(init_r, "\"<\"") != NULL);
	free(init_r);

	const char *src =
		"component LikeButton(const char *label, int initial) {\n"
		"\treturn <button class=\"btn\">{label}</button>;\n"
		"}\n"
		"component App(void) {\n"
		"\treturn (\n"
		"\t\t<div>\n"
		"\t\t\t<Li\n"
		"\t\t\t<LikeButton \n"
		"\t\t\t<button on\n"
		"\t\t</div>\n"
		"\t);\n"
		"}\n";
	char *esc = json_escape_str(src);
	char open_msg[4096];
	snprintf(open_msg, sizeof(open_msg),
	         "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"file:///tmp/demo.cx\",\"languageId\":\"cx\",\"version\":1,\"text\":\"%s\"}}}",
	         esc);
	free(esc);
	lsp_send(&c, open_msg);

	/* Consume initial cx diagnostics notification */
	char *diag0 = lsp_wait_method(&c, "textDocument/publishDiagnostics", 5000);
	assert(strstr(diag0, "\"file:///tmp/demo.cx\"") != NULL);
	assert(strstr(diag0, "\"source\":\"cx\"") != NULL);
	free(diag0);

	/* 1. Tag completion at line 6, col 6 (`\t\t\t<Li|`) -> LikeButton */
	lsp_send(&c,
	         "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/completion\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"file:///tmp/demo.cx\"},"
	         "\"position\":{\"line\":6,\"character\":6}}}");
	char *r2 = lsp_wait_id(&c, 2, 5000);
	assert(strstr(r2, "\"label\":\"LikeButton\"") != NULL);
	assert(strstr(r2, "\"detail\":\"cx component\"") != NULL);
	free(r2);

	/* 2. Component prop completion at line 7, col 15 (`\t\t\t<LikeButton |`) -> label, initial, key */
	lsp_send(&c,
	         "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/completion\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"file:///tmp/demo.cx\"},"
	         "\"position\":{\"line\":7,\"character\":15}}}");
	char *r3 = lsp_wait_id(&c, 3, 5000);
	assert(strstr(r3, "\"label\":\"label\"") != NULL);
	assert(strstr(r3, "\"detail\":\"const char *label\"") != NULL);
	assert(strstr(r3, "\"label\":\"initial\"") != NULL);
	assert(strstr(r3, "\"detail\":\"int initial\"") != NULL);
	assert(strstr(r3, "\"label\":\"key\"") != NULL);
	free(r3);

	/* 3. HTML attribute completion at line 8, col 13 (`\t\t\t<button on|`) -> onClick */
	lsp_send(&c,
	         "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"textDocument/completion\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"file:///tmp/demo.cx\"},"
	         "\"position\":{\"line\":8,\"character\":13}}}");
	char *r4 = lsp_wait_id(&c, 4, 5000);
	assert(strstr(r4, "\"label\":\"onClick\"") != NULL);
	assert(strstr(r4, "\"label\":\"class\"") == NULL); /* filtered by prefix "on" */
	free(r4);

	lsp_stop(&c);
	printf("  test_jsx_completions_and_cx_diags: PASS\n");
}

/* ── Test 2: Live `clangd` E2E (diagnostics, hover, definition, completion) */

static void test_real_clangd_e2e(const char *lsp_bin)
{
	if (access("/usr/bin/clangd", X_OK) != 0) {
		printf("  test_real_clangd_e2e: SKIP (clangd not found)\n");
		return;
	}

	char cwd[PATH_MAX];
	assert(getcwd(cwd, sizeof(cwd)) != NULL);

	char tmpdir[] = "/tmp/cx_lsp_e2e_XXXXXX";
	assert(mkdtemp(tmpdir) != NULL);

	/* Write compile_flags.txt so clangd finds <bud/bud_reconcile.h> cleanly */
	char flags_path[PATH_MAX];
	snprintf(flags_path, sizeof(flags_path), "%s/compile_flags.txt", tmpdir);
	FILE *ff = fopen(flags_path, "wb");
	assert(ff != NULL);
	fprintf(ff, "-std=c11\n-I%s/include\n", cwd);
	fclose(ff);

	char cx_path[PATH_MAX];
	snprintf(cx_path, sizeof(cx_path), "%s/sample.cx", tmpdir);
	char cx_uri[PATH_MAX + 16];
	snprintf(cx_uri, sizeof(cx_uri), "file://%s", cx_path);

	char root_uri[PATH_MAX + 16];
	snprintf(root_uri, sizeof(root_uri), "file://%s", tmpdir);

	char *argv[] = { (char *)lsp_bin, (char *)"--clangd", (char *)"/usr/bin/clangd",
	                 (char *)"--", (char *)"--log=error", NULL };
	lsp_client c;
	lsp_start(&c, lsp_bin, argv);

	char init_msg[PATH_MAX + 256];
	snprintf(init_msg, sizeof(init_msg),
	         "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
	         "\"processId\":null,\"rootUri\":\"%s\",\"capabilities\":{}}}",
	         root_uri);
	lsp_send(&c, init_msg);
	char *init_r = lsp_wait_id(&c, 1, 10000);
	assert(strstr(init_r, "\"capabilities\"") != NULL);
	assert(strstr(init_r, "\"<\"") != NULL);
	free(init_r);

	lsp_send(&c, "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}");

	/*
	 * Line numbers (0-based):
	 *  0: #include <bud/bud_reconcile.h>
	 *  1: typedef struct Track {
	 *  2:     const char *title;
	 *  3:     int bpm;
	 *  4: } Track;
	 *  5: component LikeButton(const char *label, int initial) {
	 *  6:     int *likes = bud_use_state(int, initial);
	 *  7:     return (
	 *  8:         <button onClick={[int *likes](bud_event *e) { bud_set(likes, *likes + 1); }}>
	 *  9:             {label} {*likes}
	 * 10:         </button>
	 * 11:     );
	 * 12: }
	 * 13: component SongCard(const Track *track) {
	 * 14:     return (
	 * 15:         <div class="card">
	 * 16:             <span>{track->title}</span>
	 * 17:             <LikeButton label={track->title} initial={track->bpm} />
	 * 18:         </div>
	 * 19:     );
	 * 20: }
	 */
	const char *valid_src =
		"#include <bud/bud_reconcile.h>\n"
		"typedef struct Track {\n"
		"\tconst char *title;\n"
		"\tint bpm;\n"
		"} Track;\n"
		"component LikeButton(const char *label, int initial) {\n"
		"\tint *likes = bud_use_state(int, initial);\n"
		"\treturn (\n"
		"\t\t<button onClick={[int *likes](bud_event *e) { bud_set(likes, *likes + 1); }}>\n"
		"\t\t\t{label} {*likes}\n"
		"\t\t</button>\n"
		"\t);\n"
		"}\n"
		"component SongCard(const Track *track) {\n"
		"\treturn (\n"
		"\t\t<div class=\"card\">\n"
		"\t\t\t<span>{track->title}</span>\n"
		"\t\t\t<LikeButton label={track->title} initial={track->bpm} />\n"
		"\t\t</div>\n"
		"\t);\n"
		"}\n";

	char *esc = json_escape_str(valid_src);
	char open_msg[8192];
	snprintf(open_msg, sizeof(open_msg),
	         "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"cx\",\"version\":1,\"text\":\"%s\"}}}",
	         cx_uri, esc);
	free(esc);
	lsp_send(&c, open_msg);

	/* 1. Initial publishDiagnostics must target `.cx` URI with 0 diagnostics */
	char *diag1 = lsp_wait_method(&c, "textDocument/publishDiagnostics", 10000);
	assert(strstr(diag1, cx_uri) != NULL);
	assert(strstr(diag1, ".cx.c") == NULL);
	assert(strstr(diag1, "\"diagnostics\":[]") != NULL);
	free(diag1);

	/* 2. Hover on `title` inside `{track->title}` at line 16, col 18 */
	char req[PATH_MAX + 512];
	snprintf(req, sizeof(req),
	         "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"textDocument/hover\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":16,\"character\":18}}}",
	         cx_uri);
	lsp_send(&c, req);
	char *hov = lsp_wait_id(&c, 10, 10000);
	assert(strstr(hov, "title") != NULL);
	assert(strstr(hov, "const char *") != NULL);
	free(hov);

	/* 2b. Hover on `likes` inside lifted closure body at line 8, col 58 */
	snprintf(req, sizeof(req),
	         "{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"textDocument/hover\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":8,\"character\":58}}}",
	         cx_uri);
	lsp_send(&c, req);
	char *hov_cl = lsp_wait_id(&c, 14, 10000);
	assert(strstr(hov_cl, "likes") != NULL);
	assert(strstr(hov_cl, "int *") != NULL);
	free(hov_cl);

	/* 3. Go-to-Definition on `<LikeButton` at line 17, col 6 -> line 5 (`component LikeButton`) */
	snprintf(req, sizeof(req),
	         "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"textDocument/definition\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":17,\"character\":6}}}",
	         cx_uri);
	lsp_send(&c, req);
	char *def1 = lsp_wait_id(&c, 11, 10000);
	assert(strstr(def1, cx_uri) != NULL);
	assert(strstr(def1, ".cx.c") == NULL);
	assert(strstr(def1, "\"line\":5") != NULL);
	free(def1);

	/* 4. Go-to-Definition on `bpm` inside `initial={track->bpm}` at line 17, col 53 -> line 3 (`int bpm;`) */
	snprintf(req, sizeof(req),
	         "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"textDocument/definition\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":17,\"character\":53}}}",
	         cx_uri);
	lsp_send(&c, req);
	char *def2 = lsp_wait_id(&c, 12, 10000);
	assert(strstr(def2, cx_uri) != NULL);
	assert(strstr(def2, "\"line\":3") != NULL);
	free(def2);

	/* 5. Mid-keystroke incomplete-edit C completion inside `<span class={track->` */
	const char *incomplete_src =
		"#include <bud/bud_reconcile.h>\n"
		"typedef struct Track {\n"
		"\tconst char *title;\n"
		"\tint bpm;\n"
		"} Track;\n"
		"component SongCard(const Track *track) {\n"
		"\treturn (\n"
		"\t\t<span class={track->\n"
		"\t);\n"
		"}\n";
	esc = json_escape_str(incomplete_src);
	char chg_msg[8192];
	snprintf(chg_msg, sizeof(chg_msg),
	         "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"%s\",\"version\":2},"
	         "\"contentChanges\":[{\"text\":\"%s\"}]}}",
	         cx_uri, esc);
	free(esc);
	lsp_send(&c, chg_msg);

	/* Line 7: `\t\t<span class={track->` -> col 22 is right after `->` */
	snprintf(req, sizeof(req),
	         "{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"textDocument/completion\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":7,\"character\":22}}}",
	         cx_uri);
	lsp_send(&c, req);
	char *comp = lsp_wait_id(&c, 13, 10000);
	assert(strstr(comp, "title") != NULL);
	assert(strstr(comp, "bpm") != NULL);
	free(comp);

	/* 6. Real clangd diagnostic remapped back to exact `.cx` line 7 (`track->no_such_field`) via incremental range edit */
	snprintf(chg_msg, sizeof(chg_msg),
	         "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"%s\",\"version\":3},"
	         "\"contentChanges\":[{\"range\":{\"start\":{\"line\":7,\"character\":0},"
	         "\"end\":{\"line\":7,\"character\":22}},"
	         "\"text\":\"\\t\\t<span>{track->no_such_field}</span>\"}]}}",
	         cx_uri);
	lsp_send(&c, chg_msg);

	int saw_remapped_err = 0;
	for (int k = 0; k < 16; k++) {
		char *dmsg = lsp_wait_method(&c, "textDocument/publishDiagnostics", 10000);
		if (strstr(dmsg, "no_such_field") != NULL) {
			assert(strstr(dmsg, cx_uri) != NULL);
			assert(strstr(dmsg, ".cx.c") == NULL);
			assert(strstr(dmsg, "\"line\":7") != NULL);
			saw_remapped_err = 1;
			free(dmsg);
			break;
		}
		free(dmsg);
	}
	assert(saw_remapped_err);

	lsp_stop(&c);
	unlink(flags_path);
	rmdir(tmpdir);
	printf("  test_real_clangd_e2e: PASS\n");
}

/* ── Test 3: Bucket 1 — Hover, Diagnostic Translation, Symbols, Fold, Linked, Rename */

static void test_bucket1_ux_features(const char *lsp_bin)
{
	if (access("/usr/bin/clangd", X_OK) != 0) {
		printf("  test_bucket1_ux_features: SKIP (clangd not found)\n");
		return;
	}

	char cwd[PATH_MAX];
	assert(getcwd(cwd, sizeof(cwd)) != NULL);

	char tmpdir[] = "/tmp/cx_lsp_b1_XXXXXX";
	assert(mkdtemp(tmpdir) != NULL);

	char flags_path[PATH_MAX];
	snprintf(flags_path, sizeof(flags_path), "%s/compile_flags.txt", tmpdir);
	FILE *ff = fopen(flags_path, "wb");
	assert(ff != NULL);
	fprintf(ff, "-std=c11\n-I%s/include\n", cwd);
	fclose(ff);

	char cx_path[PATH_MAX];
	snprintf(cx_path, sizeof(cx_path), "%s/b1.cx", tmpdir);
	char cx_uri[PATH_MAX + 16];
	snprintf(cx_uri, sizeof(cx_uri), "file://%s", cx_path);
	char root_uri[PATH_MAX + 16];
	snprintf(root_uri, sizeof(root_uri), "file://%s", tmpdir);

	char *argv[] = { (char *)lsp_bin, (char *)"--clangd", (char *)"/usr/bin/clangd",
	                 (char *)"--", (char *)"--log=error", NULL };
	lsp_client c;
	lsp_start(&c, lsp_bin, argv);

	char msg[8192];
	snprintf(msg, sizeof(msg),
	         "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
	         "\"processId\":null,\"rootUri\":\"%s\",\"capabilities\":{}}}",
	         root_uri);
	lsp_send(&c, msg);
	char *init_r = lsp_wait_id(&c, 1, 10000);
	assert(strstr(init_r, "\"foldingRangeProvider\":true") != NULL);
	assert(strstr(init_r, "\"linkedEditingRangeProvider\":true") != NULL);
	assert(strstr(init_r, "\"documentSymbolProvider\":true") != NULL);
	assert(strstr(init_r, "\"codeActionProvider\":true") != NULL);
	assert(strstr(init_r, "\"prepareProvider\":true") != NULL);
	assert(strstr(init_r, "\"semanticTokensProvider\"") == NULL);
	free(init_r);

	lsp_send(&c, "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}");

	/*
	 *  0: #include <bud/bud_reconcile.h>
	 *  1: typedef struct Track { const char *title; } Track;
	 *  2: /// Interactive like counter button.
	 *  3: component LikeButton(const char *label, int initial) {
	 *  4:     int *likes = bud_use_state(int, initial);
	 *  5:     return (
	 *  6:         <button onClick={[int *likes](bud_event *e) { bud_set(likes, *likes + 1); }}>
	 *  7:             {label} {*likes}
	 *  8:         </button>
	 *  9:     );
	 * 10: }
	 * 11: component App(void) {
	 * 12:     return (
	 * 13:         <div class="a" class="b">
	 * 14:             <LikeButton label="Go" initial={1} bogus={2} />
	 * 15:             {for (int i = 0; i < 3; i++) <li>{i}</li>}
	 * 16:         </div>
	 * 17:     );
	 * 18: }
	 */
	const char *src =
		"#include <bud/bud_reconcile.h>\n"
		"typedef struct Track { const char *title; } Track;\n"
		"/// Interactive like counter button.\n"
		"component LikeButton(const char *label, int initial) {\n"
		"\tint *likes = bud_use_state(int, initial);\n"
		"\treturn (\n"
		"\t\t<button onClick={[int *likes](bud_event *e) { bud_set(likes, *likes + 1); }}>\n"
		"\t\t\t{label} {*likes}\n"
		"\t\t</button>\n"
		"\t);\n"
		"}\n"
		"component App(void) {\n"
		"\treturn (\n"
		"\t\t<div class=\"a\" class=\"b\">\n"
		"\t\t\t<LikeButton label=\"Go\" initial={1} bogus={2} />\n"
		"\t\t\t{for (int i = 0; i < 3; i++) <li>{i}</li>}\n"
		"\t\t</div>\n"
		"\t);\n"
		"}\n";

	char *esc = json_escape_str(src);
	snprintf(msg, sizeof(msg),
	         "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"cx\",\"version\":1,\"text\":\"%s\"}}}",
	         cx_uri, esc);
	free(esc);
	lsp_send(&c, msg);

	/* 1. Diagnostics: translated unknown prop + cx-missing-key + cx-duplicate-attr */
	char *dmsg = lsp_wait_method(&c, "textDocument/publishDiagnostics", 10000);
	assert(strstr(dmsg, "unknown prop 'bogus' on component <LikeButton>") != NULL);
	assert(strstr(dmsg, "LikeButton_props") == NULL);
	assert(strstr(dmsg, "cx-missing-key") != NULL);
	assert(strstr(dmsg, "cx-duplicate-attr") != NULL);
	free(dmsg);

	/* 2. Native component hover on `<LikeButton` (line 14, col 6) */
	snprintf(msg, sizeof(msg),
	         "{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"textDocument/hover\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":14,\"character\":6}}}",
	         cx_uri);
	lsp_send(&c, msg);
	char *h1 = lsp_wait_id(&c, 20, 10000);
	assert(strstr(h1, "component LikeButton") != NULL);
	assert(strstr(h1, "Interactive like counter button.") != NULL);
	assert(strstr(h1, "_props") == NULL);
	assert(strstr(h1, "_render") == NULL);
	free(h1);

	/* 3. Native prop hover on `label=` (line 14, col 16) */
	snprintf(msg, sizeof(msg),
	         "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"textDocument/hover\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":14,\"character\":16}}}",
	         cx_uri);
	lsp_send(&c, msg);
	char *h2 = lsp_wait_id(&c, 21, 10000);
	assert(strstr(h2, "const char *label") != NULL);
	free(h2);

	/* 4. Native HTML event hover on `onClick` (line 6, col 12) */
	snprintf(msg, sizeof(msg),
	         "{\"jsonrpc\":\"2.0\",\"id\":22,\"method\":\"textDocument/hover\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":6,\"character\":12}}}",
	         cx_uri);
	lsp_send(&c, msg);
	char *h3 = lsp_wait_id(&c, 22, 10000);
	assert(strstr(h3, "onClick") != NULL);
	assert(strstr(h3, "bud_event") != NULL);
	free(h3);

	/* 5. documentSymbol: clean component + C symbols, zero shadow boilerplate */
	snprintf(msg, sizeof(msg),
	         "{\"jsonrpc\":\"2.0\",\"id\":23,\"method\":\"textDocument/documentSymbol\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"%s\"}}}",
	         cx_uri);
	lsp_send(&c, msg);
	char *syms = lsp_wait_id(&c, 23, 10000);
	assert(strstr(syms, "\"name\":\"LikeButton\"") != NULL);
	assert(strstr(syms, "\"name\":\"App\"") != NULL);
	assert(strstr(syms, "\"name\":\"Track\"") != NULL);
	assert(strstr(syms, "LikeButton_props") == NULL);
	assert(strstr(syms, "LikeButton_render") == NULL);
	assert(strstr(syms, "__cx_") == NULL);
	free(syms);

	/* 6. foldingRange */
	snprintf(msg, sizeof(msg),
	         "{\"jsonrpc\":\"2.0\",\"id\":24,\"method\":\"textDocument/foldingRange\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"%s\"}}}",
	         cx_uri);
	lsp_send(&c, msg);
	char *folds = lsp_wait_id(&c, 24, 10000);
	assert(strstr(folds, "\"startLine\":3") != NULL);
	assert(strstr(folds, "\"endLine\":10") != NULL);
	assert(strstr(folds, "\"startLine\":13") != NULL);
	assert(strstr(folds, "\"endLine\":16") != NULL);
	free(folds);

	/* 7. linkedEditingRange on `<div` (line 13, col 4) -> lines 13 & 16 */
	snprintf(msg, sizeof(msg),
	         "{\"jsonrpc\":\"2.0\",\"id\":25,\"method\":\"textDocument/linkedEditingRange\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":13,\"character\":4}}}",
	         cx_uri);
	lsp_send(&c, msg);
	char *lnk = lsp_wait_id(&c, 25, 10000);
	assert(strstr(lnk, "\"line\":13") != NULL);
	assert(strstr(lnk, "\"line\":16") != NULL);
	free(lnk);

	/* 8. prepareRename & rename on `<LikeButton` (line 14, col 6) -> renames line 3 and line 14 */
	snprintf(msg, sizeof(msg),
	         "{\"jsonrpc\":\"2.0\",\"id\":26,\"method\":\"textDocument/prepareRename\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":14,\"character\":6}}}",
	         cx_uri);
	lsp_send(&c, msg);
	char *pr = lsp_wait_id(&c, 26, 10000);
	assert(strstr(pr, "\"line\":14") != NULL);
	free(pr);

	snprintf(msg, sizeof(msg),
	         "{\"jsonrpc\":\"2.0\",\"id\":27,\"method\":\"textDocument/rename\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":14,\"character\":6},"
	         "\"newName\":\"StarButton\"}}",
	         cx_uri);
	lsp_send(&c, msg);
	char *ren = lsp_wait_id(&c, 27, 10000);
	assert(strstr(ren, cx_uri) != NULL);
	assert(strstr(ren, ".cx.c") == NULL);
	assert(strstr(ren, "\"newText\":\"StarButton\"") != NULL);
	assert(strstr(ren, "\"line\":3") != NULL);
	assert(strstr(ren, "\"line\":14") != NULL);
	free(ren);

	/* 9. Safe C local variable rename on `likes` (line 4, col 7) */
	snprintf(msg, sizeof(msg),
	         "{\"jsonrpc\":\"2.0\",\"id\":28,\"method\":\"textDocument/rename\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":4,\"character\":7},"
	         "\"newName\":\"votes\"}}",
	         cx_uri);
	lsp_send(&c, msg);
	char *ren2 = lsp_wait_id(&c, 28, 10000);
	assert(strstr(ren2, cx_uri) != NULL);
	assert(strstr(ren2, ".cx.c") == NULL);
	assert(strstr(ren2, "\"newText\":\"votes\"") != NULL);
	assert(strstr(ren2, "\"line\":4") != NULL);
	free(ren2);

	lsp_stop(&c);
	unlink(flags_path);
	rmdir(tmpdir);
	printf("  test_bucket1_ux_features: PASS\n");
}

/* ── Test 4: Bucket 2 — Multi-file `.cxh`/`.cx`/`.cx.h` & Zero-Config fallbackFlags */

static void test_bucket2_crossfile_and_zero_config(const char *lsp_bin)
{
	if (access("/usr/bin/clangd", X_OK) != 0) {
		printf("  test_bucket2_crossfile_and_zero_config: SKIP (clangd not found)\n");
		return;
	}

	char cwd[PATH_MAX];
	assert(getcwd(cwd, sizeof(cwd)) != NULL);

	char tmpdir[] = "/tmp/cx_lsp_b2_XXXXXX";
	assert(mkdtemp(tmpdir) != NULL);

	/* NOTE: NO compile_flags.txt here! Symlink `include` so zero-config fallbackFlags finds <bud/...> */
	char inc_src[PATH_MAX + 16], inc_dst[PATH_MAX + 16];
	snprintf(inc_src, sizeof(inc_src), "%s/include", cwd);
	snprintf(inc_dst, sizeof(inc_dst), "%s/include", tmpdir);
	assert(symlink(inc_src, inc_dst) == 0);

	char cxh_path[PATH_MAX], btn_path[PATH_MAX], app_path[PATH_MAX];
	char badge_cx[PATH_MAX], badge_h[PATH_MAX];
	snprintf(cxh_path, sizeof(cxh_path), "%s/button.cxh", tmpdir);
	snprintf(btn_path, sizeof(btn_path), "%s/button.cx", tmpdir);
	snprintf(app_path, sizeof(app_path), "%s/app.cx", tmpdir);
	snprintf(badge_cx, sizeof(badge_cx), "%s/badge.cx", tmpdir);
	snprintf(badge_h, sizeof(badge_h), "%s/badge.cx.h", tmpdir);

	FILE *f = fopen(cxh_path, "wb");
	assert(f != NULL);
	fputs("#pragma once\n"
	      "/// Primary action button.\n"
	      "component Button(const char *label, int disabled);\n", f);
	fclose(f);

	f = fopen(btn_path, "wb");
	assert(f != NULL);
	fputs("#include <bud/bud_reconcile.h>\n"
	      "#include \"button.cxh\"\n"
	      "/// Primary action button.\n"
	      "component Button(const char *label, int disabled) {\n"
	      "\treturn <button disabled={disabled}>{label}</button>;\n"
	      "}\n", f);
	fclose(f);

	f = fopen(badge_cx, "wb");
	assert(f != NULL);
	fputs("#include <bud/bud_reconcile.h>\n"
	      "component Badge(const char *text) {\n"
	      "\treturn <span>{text}</span>;\n"
	      "}\n", f);
	fclose(f);

	/* Simulate `cx --emit-header badge.cx.h badge.cx` */
	f = fopen(badge_h, "wb");
	assert(f != NULL);
	fputs("#pragma once\n"
	      "#include <bud/bud_reconcile.h>\n"
	      "typedef struct Badge_props { const char *text; } Badge_props;\n"
	      "bud_node *Badge_render(void *__cx_ctx, const void *__cx_p);\n", f);
	fclose(f);

	/*
	 *  0: #include <bud/bud_reconcile.h>
	 *  1: #include "button.cxh"
	 *  2: #include "badge.cx.h"
	 *  3: component App(void) {
	 *  4:     return (
	 *  5:         <div>
	 *  6:             <Button label="Go" disabled={0} />
	 *  7:             <Badge text="ok" />
	 *  8:         </div>
	 *  9:     );
	 * 10: }
	 */
	const char *app_src =
		"#include <bud/bud_reconcile.h>\n"
		"#include \"button.cxh\"\n"
		"#include \"badge.cx.h\"\n"
		"component App(void) {\n"
		"\treturn (\n"
		"\t\t<div>\n"
		"\t\t\t<Button label=\"Go\" disabled={0} />\n"
		"\t\t\t<Badge text=\"ok\" />\n"
		"\t\t</div>\n"
		"\t);\n"
		"}\n";
	f = fopen(app_path, "wb");
	assert(f != NULL);
	fputs(app_src, f);
	fclose(f);

	char app_uri[PATH_MAX + 16], btn_uri[PATH_MAX + 16], badge_uri[PATH_MAX + 16];
	char root_uri[PATH_MAX + 16];
	snprintf(app_uri, sizeof(app_uri), "file://%s", app_path);
	snprintf(btn_uri, sizeof(btn_uri), "file://%s", btn_path);
	snprintf(badge_uri, sizeof(badge_uri), "file://%s", badge_cx);
	snprintf(root_uri, sizeof(root_uri), "file://%s", tmpdir);

	char *argv[] = { (char *)lsp_bin, (char *)"--clangd", (char *)"/usr/bin/clangd",
	                 (char *)"--", (char *)"--log=error", NULL };
	lsp_client c;
	lsp_start(&c, lsp_bin, argv);

	char msg[8192];
	snprintf(msg, sizeof(msg),
	         "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
	         "\"processId\":null,\"rootUri\":\"%s\",\"capabilities\":{}}}",
	         root_uri);
	lsp_send(&c, msg);
	char *init_r = lsp_wait_id(&c, 1, 10000);
	assert(strstr(init_r, "\"capabilities\"") != NULL);
	free(init_r);

	lsp_send(&c, "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}");

	char *esc = json_escape_str(app_src);
	snprintf(msg, sizeof(msg),
	         "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"cx\",\"version\":1,\"text\":\"%s\"}}}",
	         app_uri, esc);
	free(esc);
	lsp_send(&c, msg);

	/* 1. Zero-config clangd + spliced `#include "button.cxh"` => 0 diagnostics! */
	char *d0 = lsp_wait_method(&c, "textDocument/publishDiagnostics", 10000);
	if (strstr(d0, "\"diagnostics\":[]") == NULL)
		fprintf(stderr, "bucket2 unexpected diags: %s\n", d0);
	assert(strstr(d0, app_uri) != NULL);
	assert(strstr(d0, "\"diagnostics\":[]") != NULL);
	free(d0);

	/* 2. Cross-file Go-to-Definition on `<Button` (line 6, col 6) -> button.cx (prefers def over .cxh) */
	snprintf(msg, sizeof(msg),
	         "{\"jsonrpc\":\"2.0\",\"id\":30,\"method\":\"textDocument/definition\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":6,\"character\":6}}}",
	         app_uri);
	lsp_send(&c, msg);
	char *def_btn = lsp_wait_id(&c, 30, 10000);
	assert(strstr(def_btn, btn_uri) != NULL);
	assert(strstr(def_btn, "\"line\":3") != NULL);
	free(def_btn);

	/* 3. Cross-file Go-to-Definition on `<Badge` (line 7, col 6) -> badge.cx (not badge.cx.h) */
	snprintf(msg, sizeof(msg),
	         "{\"jsonrpc\":\"2.0\",\"id\":31,\"method\":\"textDocument/definition\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":7,\"character\":6}}}",
	         app_uri);
	lsp_send(&c, msg);
	char *def_badge = lsp_wait_id(&c, 31, 10000);
	assert(strstr(def_badge, badge_uri) != NULL);
	assert(strstr(def_badge, "badge.cx.h") == NULL);
	free(def_badge);

	/* 4. Cross-file hover on `<Button` shows doc from workspace index */
	snprintf(msg, sizeof(msg),
	         "{\"jsonrpc\":\"2.0\",\"id\":32,\"method\":\"textDocument/hover\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":6,\"character\":6}}}",
	         app_uri);
	lsp_send(&c, msg);
	char *hbtn = lsp_wait_id(&c, 32, 10000);
	assert(strstr(hbtn, "Primary action button.") != NULL);
	assert(strstr(hbtn, "const char *label") != NULL);
	free(hbtn);

	/* 5. Cross-file `<Bu|` and `<Button |` completion */
	const char *app_edit =
		"#include <bud/bud_reconcile.h>\n"
		"#include \"button.cxh\"\n"
		"component App(void) {\n"
		"\treturn (\n"
		"\t\t<div>\n"
		"\t\t\t<Bu\n"
		"\t\t\t<Button \n"
		"\t\t</div>\n"
		"\t);\n"
		"}\n";
	esc = json_escape_str(app_edit);
	snprintf(msg, sizeof(msg),
	         "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"%s\",\"version\":2},"
	         "\"contentChanges\":[{\"text\":\"%s\"}]}}",
	         app_uri, esc);
	free(esc);
	lsp_send(&c, msg);

	snprintf(msg, sizeof(msg),
	         "{\"jsonrpc\":\"2.0\",\"id\":33,\"method\":\"textDocument/completion\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":5,\"character\":6}}}",
	         app_uri);
	lsp_send(&c, msg);
	char *c1 = lsp_wait_id(&c, 33, 10000);
	assert(strstr(c1, "\"label\":\"Button\"") != NULL);
	free(c1);

	snprintf(msg, sizeof(msg),
	         "{\"jsonrpc\":\"2.0\",\"id\":34,\"method\":\"textDocument/completion\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":6,\"character\":11}}}",
	         app_uri);
	lsp_send(&c, msg);
	char *c2 = lsp_wait_id(&c, 34, 10000);
	assert(strstr(c2, "\"label\":\"disabled\"") != NULL);
	assert(strstr(c2, "\"detail\":\"int disabled\"") != NULL);
	free(c2);

	lsp_stop(&c);
	unlink(cxh_path);
	unlink(btn_path);
	unlink(app_path);
	unlink(badge_cx);
	unlink(badge_h);
	unlink(inc_dst);
	rmdir(tmpdir);
	printf("  test_bucket2_crossfile_and_zero_config: PASS\n");
}

/* ── Test 5: Bucket 3 — Smart Code Actions & Quick-Fix Round-Trip ─────── */

static void test_bucket3_code_actions(const char *lsp_bin)
{
	if (access("/usr/bin/clangd", X_OK) != 0) {
		printf("  test_bucket3_code_actions: SKIP (clangd not found)\n");
		return;
	}

	char cwd[PATH_MAX];
	assert(getcwd(cwd, sizeof(cwd)) != NULL);

	char tmpdir[] = "/tmp/cx_lsp_b3_XXXXXX";
	assert(mkdtemp(tmpdir) != NULL);

	char flags_path[PATH_MAX];
	snprintf(flags_path, sizeof(flags_path), "%s/compile_flags.txt", tmpdir);
	FILE *ff = fopen(flags_path, "wb");
	assert(ff != NULL);
	fprintf(ff, "-std=c11\n-I%s/include\n", cwd);
	fclose(ff);

	char cx_path[PATH_MAX];
	snprintf(cx_path, sizeof(cx_path), "%s/b3.cx", tmpdir);
	char cx_uri[PATH_MAX + 16];
	snprintf(cx_uri, sizeof(cx_uri), "file://%s", cx_path);
	char root_uri[PATH_MAX + 16];
	snprintf(root_uri, sizeof(root_uri), "file://%s", tmpdir);

	char *argv[] = { (char *)lsp_bin, (char *)"--clangd", (char *)"/usr/bin/clangd",
	                 (char *)"--", (char *)"--log=error", NULL };
	lsp_client c;
	lsp_start(&c, lsp_bin, argv);

	char msg[8192];
	snprintf(msg, sizeof(msg),
	         "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
	         "\"processId\":null,\"rootUri\":\"%s\",\"capabilities\":{}}}",
	         root_uri);
	lsp_send(&c, msg);
	char *init_r = lsp_wait_id(&c, 1, 10000);
	free(init_r);
	lsp_send(&c, "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}");

	/*
	 *  0: #include <bud/bud_reconcile.h>
	 *  1: component LikeButton(const char *label, int initial) {
	 *  2:     int *likes = bud_use_state(int, initial);
	 *  3:     return <button onClick={[](bud_event *e) { bud_set(likes, *likes + 1); }}>{label}</button>;
	 *  4: }
	 *  5: component App(void) {
	 *  6:     return (
	 *  7:         <div class="a" class="b">
	 *  8:             <LikeButton />
	 *  9:             <NewCard title="hi" />
	 * 10:             {for (int i = 0; i < 3; i++) <li>{i}</li>}
	 * 11:         </div>
	 * 12:     );
	 * 13: }
	 */
	const char *src =
		"#include <bud/bud_reconcile.h>\n"
		"component LikeButton(const char *label, int initial) {\n"
		"\tint *likes = bud_use_state(int, initial);\n"
		"\treturn <button onClick={[](bud_event *e) { bud_set(likes, *likes + 1); }}>{label}</button>;\n"
		"}\n"
		"component App(void) {\n"
		"\treturn (\n"
		"\t\t<div class=\"a\" class=\"b\">\n"
		"\t\t\t<LikeButton />\n"
		"\t\t\t<NewCard title=\"hi\" />\n"
		"\t\t\t{for (int i = 0; i < 3; i++) <li>{i}</li>}\n"
		"\t\t</div>\n"
		"\t);\n"
		"}\n";
	char *esc = json_escape_str(src);
	snprintf(msg, sizeof(msg),
	         "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"cx\",\"version\":1,\"text\":\"%s\"}}}",
	         cx_uri, esc);
	free(esc);
	lsp_send(&c, msg);

	/* Wait for diagnostics reporting undeclared `likes`, `cx-missing-key`, `cx-duplicate-attr` */
	char *d0 = lsp_wait_method(&c, "textDocument/publishDiagnostics", 10000);
	assert(strstr(d0, "likes") != NULL);
	assert(strstr(d0, "cx-missing-key") != NULL);
	assert(strstr(d0, "cx-duplicate-attr") != NULL);
	free(d0);

	/* 1. Quick-fix: Capture 'likes' in closure (line 3) */
	snprintf(msg, sizeof(msg),
	         "{\"jsonrpc\":\"2.0\",\"id\":40,\"method\":\"textDocument/codeAction\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"%s\"},"
	         "\"range\":{\"start\":{\"line\":3,\"character\":50},\"end\":{\"line\":3,\"character\":55}},"
	         "\"context\":{\"diagnostics\":[{\"range\":{\"start\":{\"line\":3,\"character\":50},\"end\":{\"line\":3,\"character\":55}},"
	         "\"message\":\"use of undeclared identifier 'likes'\"}]}}}",
	         cx_uri);
	lsp_send(&c, msg);
	char *ca1 = lsp_wait_id(&c, 40, 10000);
	assert(strstr(ca1, "Capture 'likes' in closure [int *likes]") != NULL);
	assert(strstr(ca1, "\"newText\":\"int *likes\"") != NULL);
	free(ca1);

	/* 2. Quick-fix: Add missing props to <LikeButton /> (line 8) */
	snprintf(msg, sizeof(msg),
	         "{\"jsonrpc\":\"2.0\",\"id\":41,\"method\":\"textDocument/codeAction\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"%s\"},"
	         "\"range\":{\"start\":{\"line\":8,\"character\":5},\"end\":{\"line\":8,\"character\":10}},"
	         "\"context\":{\"diagnostics\":[]}}}",
	         cx_uri);
	lsp_send(&c, msg);
	char *ca2 = lsp_wait_id(&c, 41, 10000);
	assert(strstr(ca2, "Add 2 missing props to <LikeButton>") != NULL);
	assert(strstr(ca2, "label=\\\"\\\"") != NULL);
	assert(strstr(ca2, "initial={0}") != NULL);
	free(ca2);

	/* 3. Quick-fix: Create component NewCard (line 9) */
	snprintf(msg, sizeof(msg),
	         "{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"textDocument/codeAction\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"%s\"},"
	         "\"range\":{\"start\":{\"line\":9,\"character\":5},\"end\":{\"line\":9,\"character\":12}},"
	         "\"context\":{\"diagnostics\":[{\"range\":{\"start\":{\"line\":9,\"character\":4},\"end\":{\"line\":9,\"character\":11}},"
	         "\"message\":\"use of undeclared identifier 'NewCard_render'\"}]}}}",
	         cx_uri);
	lsp_send(&c, msg);
	char *ca3 = lsp_wait_id(&c, 42, 10000);
	assert(strstr(ca3, "Create component NewCard") != NULL);
	assert(strstr(ca3, "component NewCard(const char *title)") != NULL);
	free(ca3);

	/* 4. Quick-fix: Add key={i} on <li> inside loop (line 10) */
	snprintf(msg, sizeof(msg),
	         "{\"jsonrpc\":\"2.0\",\"id\":43,\"method\":\"textDocument/codeAction\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"%s\"},"
	         "\"range\":{\"start\":{\"line\":10,\"character\":32},\"end\":{\"line\":10,\"character\":36}},"
	         "\"context\":{\"diagnostics\":[{\"range\":{\"start\":{\"line\":10,\"character\":32},\"end\":{\"line\":10,\"character\":36}},"
	         "\"code\":\"cx-missing-key\",\"message\":\"missing key\"}]}}}",
	         cx_uri);
	lsp_send(&c, msg);
	char *ca4 = lsp_wait_id(&c, 43, 10000);
	assert(strstr(ca4, "Add key={i}") != NULL);
	assert(strstr(ca4, " key={i}") != NULL);
	free(ca4);

	/* 5. Quick-fix: Remove duplicate attribute 'class' (line 7) */
	snprintf(msg, sizeof(msg),
	         "{\"jsonrpc\":\"2.0\",\"id\":44,\"method\":\"textDocument/codeAction\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"%s\"},"
	         "\"range\":{\"start\":{\"line\":7,\"character\":17},\"end\":{\"line\":7,\"character\":22}},"
	         "\"context\":{\"diagnostics\":[{\"range\":{\"start\":{\"line\":7,\"character\":17},\"end\":{\"line\":7,\"character\":22}},"
	         "\"code\":\"cx-duplicate-attr\",\"message\":\"duplicate attribute 'class'\"}]}}}",
	         cx_uri);
	lsp_send(&c, msg);
	char *ca5 = lsp_wait_id(&c, 44, 10000);
	assert(strstr(ca5, "Remove duplicate attribute 'class'") != NULL);
	free(ca5);

	/* 6. Apply the closure capture fix via didChange on a clean doc and verify 0 clangd diags! */
	const char *fixed_src =
		"#include <bud/bud_reconcile.h>\n"
		"component LikeButton(const char *label, int initial) {\n"
		"\tint *likes = bud_use_state(int, initial);\n"
		"\treturn <button onClick={[int *likes](bud_event *e) { bud_set(likes, *likes + 1); }}>{label}</button>;\n"
		"}\n";
	esc = json_escape_str(fixed_src);
	snprintf(msg, sizeof(msg),
	         "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{"
	         "\"textDocument\":{\"uri\":\"%s\",\"version\":2},"
	         "\"contentChanges\":[{\"text\":\"%s\"}]}}",
	         cx_uri, esc);
	free(esc);
	lsp_send(&c, msg);

	int saw_clean = 0;
	for (int k = 0; k < 16; k++) {
		char *dk = lsp_wait_method(&c, "textDocument/publishDiagnostics", 10000);
		if (strstr(dk, "\"diagnostics\":[]") != NULL) {
			saw_clean = 1;
			free(dk);
			break;
		}
		free(dk);
	}
	assert(saw_clean);

	lsp_stop(&c);
	unlink(flags_path);
	rmdir(tmpdir);
	printf("  test_bucket3_code_actions: PASS\n");
}

int main(int argc, char **argv)
{
	const char *lsp_bin = (argc > 1) ? argv[1] : "./bin/cx-lsp";
	printf("cx_lsp_test:\n");
	test_jsx_completions_and_cx_diags(lsp_bin);
	test_real_clangd_e2e(lsp_bin);
	test_bucket1_ux_features(lsp_bin);
	test_bucket2_crossfile_and_zero_config(lsp_bin);
	test_bucket3_code_actions(lsp_bin);
	printf("cx_lsp_test: ALL TESTS PASSED\n");
	return 0;
}
