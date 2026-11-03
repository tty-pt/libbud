FOLDER := bud
all := libbud bud_test

LDLIBS-libbud :=
libbud-obj-y :=
LDLIBS-bud_test := -lbud

WASM_SRCS := src/libbud.c npm/wasm/bud_wasm.c
WASM_EXPORTS := bud_element bud_text bud_set_attr bud_append bud_render_html bud_vdom_diff bud_free

include ../mk/include.mk
LDLIBS-alpine :=

${DESTDIR}${PREFIX}/lib/pkgconfig/bud.pc: bud.pc
	install -d ${DESTDIR}${PREFIX}/lib/pkgconfig
	install -m 644 bud.pc $@

install: ${DESTDIR}${PREFIX}/lib/pkgconfig/bud.pc

CFLAGS += $(EXTRA_CFLAGS)

test: all
	LD_LIBRARY_PATH=./lib ./bin/bud_test

# Optional Node.js & WebAssembly bindings
NPM ?= npm

npm-build:
	@if command -v $(NPM) >/dev/null 2>&1; then \
		(cd npm && $(NPM) run build); \
	else \
		echo "Skipping npm build — $(NPM) not found"; \
	fi

npm-test: npm-build
	@if command -v $(NPM) >/dev/null 2>&1; then \
		(cd npm && $(NPM) test); \
	fi

objects-set.mk: Makefile

.PHONY: all test npm-build npm-test

