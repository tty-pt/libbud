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

int main(int argc, char **argv)
{
	const char *lsp_bin = (argc > 1) ? argv[1] : "./bin/cx-lsp";
	printf("cx_lsp_test:\n");
	test_jsx_completions_and_cx_diags(lsp_bin);
	test_real_clangd_e2e(lsp_bin);
	printf("cx_lsp_test: ALL TESTS PASSED\n");
	return 0;
}
