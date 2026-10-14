CC ?= cc
PYTHON ?= python3
CPPFLAGS += -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror
SOURCES = src/sparseget.c src/ui.c

.PHONY: all check clean sanitize
all: sparseget

sparseget: $(SOURCES) src/ui.h Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) -o $@ $(LDFLAGS)

check: sparseget
	$(PYTHON) tests/test_sparseget.py ./sparseget

sparseget-asan: $(SOURCES) src/ui.h Makefile
	$(CC) $(CPPFLAGS) -O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(SOURCES) -o $@ $(LDFLAGS) -fsanitize=address,undefined

sanitize: sparseget-asan
	ASAN_OPTIONS=detect_leaks=0 SPARSEGET_SANITIZED=1 $(PYTHON) tests/test_sparseget.py ./sparseget-asan

clean:
	rm -f sparseget sparseget-asan
