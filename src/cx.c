/**
 * @file cx_main.c
 * @brief CLI wrapper for the `.cx` to C11 transpiler (`cx`).
 *
 * Usage:
 *   cx <input.cx> [-o <output.c>] [--map <output.cx.map>] [--no-line] [--recover]
 */

#include <bud/cx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_entire_file(const char *path, size_t *out_len)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return NULL;
	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return NULL;
	}
	long sz = ftell(fp);
	if (sz < 0) {
		fclose(fp);
		return NULL;
	}
	rewind(fp);
	char *buf = (char *)malloc((size_t)sz + 1);
	if (!buf) {
		fclose(fp);
		return NULL;
	}
	size_t nread = fread(buf, 1, (size_t)sz, fp);
	fclose(fp);
	buf[nread] = '\0';
	*out_len = nread;
	return buf;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
	        "Usage: %s <input.cx> [-o <output.c>] [--map <output.map.json>] [--emit-header <output.h>] [--no-line] [--recover]\n",
	        argv0);
}

int main(int argc, char **argv)
{
	const char *in_path = NULL;
	const char *out_path = NULL;
	const char *map_path = NULL;
	const char *hdr_path = NULL;
	int emit_lines = 1;
	int recover = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
			out_path = argv[++i];
		} else if (strcmp(argv[i], "--map") == 0 && i + 1 < argc) {
			map_path = argv[++i];
		} else if (strcmp(argv[i], "--emit-header") == 0 && i + 1 < argc) {
			hdr_path = argv[++i];
		} else if (strcmp(argv[i], "--no-line") == 0) {
			emit_lines = 0;
		} else if (strcmp(argv[i], "--recover") == 0) {
			recover = 1;
		} else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			return 0;
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "cx: unknown option '%s'\n", argv[i]);
			usage(argv[0]);
			return 2;
		} else if (!in_path) {
			in_path = argv[i];
		} else {
			fprintf(stderr, "cx: unexpected argument '%s'\n", argv[i]);
			usage(argv[0]);
			return 2;
		}
	}

	if (!in_path) {
		usage(argv[0]);
		return 2;
	}

	size_t src_len = 0;
	char *src = read_entire_file(in_path, &src_len);
	if (!src) {
		fprintf(stderr, "cx: cannot read '%s'\n", in_path);
		return 1;
	}

	cx_options opt;
	memset(&opt, 0, sizeof(opt));
	opt.filename = in_path;
	opt.emit_lines = emit_lines;
	opt.recover = recover;
	opt.build_index = (hdr_path != NULL) ? 1 : 0;

	cx_result res;
	if (cx_transpile(&opt, src, src_len, &res) != 0) {
		fprintf(stderr, "cx: internal transpiler error\n");
		free(src);
		return 1;
	}

	for (size_t i = 0; i < res.diag_count; i++) {
		const cx_diag *d = &res.diags[i];
		fprintf(stderr, "%s:%u:%u: error: %s\n",
		        in_path, (unsigned)d->line, (unsigned)d->col, d->msg);
	}

	int rc = res.ok ? 0 : 1;
	if (res.ok || recover) {
		if (out_path) {
			FILE *fp = fopen(out_path, "wb");
			if (!fp) {
				fprintf(stderr, "cx: cannot write '%s'\n", out_path);
				rc = 1;
			} else {
				if (res.c_len > 0)
					fwrite(res.c_code, 1, res.c_len, fp);
				fclose(fp);
			}
		} else if (!hdr_path) {
			if (res.c_len > 0)
				fwrite(res.c_code, 1, res.c_len, stdout);
		}
		if (map_path && cx_write_map_json(&res, map_path) != 0) {
			fprintf(stderr, "cx: cannot write span map '%s'\n", map_path);
			rc = 1;
		}
		if (hdr_path && cx_write_header(&res, src, hdr_path) != 0) {
			fprintf(stderr, "cx: cannot write header '%s'\n", hdr_path);
			rc = 1;
		}
	}

	cx_result_free(&res);
	free(src);
	return rc;
}
