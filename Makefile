FOLDER := bud
all := libbud bud_test

LDLIBS-libbud :=
libbud-obj-y := src/bud_wasm_app.o
LDLIBS-bud_test := -lbud

include ../mk/include.mk
LDLIBS-alpine :=

${DESTDIR}${PREFIX}/lib/pkgconfig/bud.pc: bud.pc
	install -d ${DESTDIR}${PREFIX}/lib/pkgconfig
	install -m 644 bud.pc $@

install: ${DESTDIR}${PREFIX}/lib/pkgconfig/bud.pc

CFLAGS += $(EXTRA_CFLAGS)

test: all
	LD_LIBRARY_PATH=./lib ./bin/bud_test

objects-set.mk: Makefile

.PHONY: all test

