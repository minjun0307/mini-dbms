CC      ?= gcc
CFLAGS  ?= -O2 -g -std=c11 -Wall -Wextra
LDFLAGS ?=
# 항상 필요한 플래그는 따로 둔다 (make CFLAGS=... 로 덮어써도 유지되도록)
ALL_CFLAGS  = $(CFLAGS) -D_GNU_SOURCE -pthread -Isrc
ALL_LDFLAGS = $(LDFLAGS) -pthread

CORE    := src/util.c src/lexer.c src/parser.c src/bptree.c src/table.c src/executor.c
HEADERS := $(wildcard src/*.h)
TESTS   := tests/test_main.c tests/test_lexer.c tests/test_parser.c tests/test_bptree.c tests/test_engine.c

.PHONY: all test unit api bench asan clean

all: build/mdb build/mdb_server build/bench build/unit_tests

build:
	mkdir -p build

build/mdb: src/cli.c $(CORE) $(HEADERS) | build
	$(CC) $(ALL_CFLAGS) -o $@ src/cli.c $(CORE) $(ALL_LDFLAGS)

build/mdb_server: src/server.c src/threadpool.c $(CORE) $(HEADERS) | build
	$(CC) $(ALL_CFLAGS) -o $@ src/server.c src/threadpool.c $(CORE) $(ALL_LDFLAGS)

build/bench: tools/bench.c $(CORE) $(HEADERS) | build
	$(CC) $(ALL_CFLAGS) -o $@ tools/bench.c $(CORE) $(ALL_LDFLAGS)

build/unit_tests: $(TESTS) tests/test.h $(CORE) $(HEADERS) | build
	$(CC) $(ALL_CFLAGS) -Itests -o $@ $(TESTS) $(CORE) $(ALL_LDFLAGS)

# 단위 테스트 + API 기능 테스트
test: unit api

unit: build/unit_tests
	./build/unit_tests

api: build/mdb_server
	python3 tests/api_test.py

bench: build/bench
	./build/bench -n 1000000

# 메모리 오류 / 정의되지 않은 동작 검사용 빌드
asan:
	$(MAKE) clean
	$(MAKE) CFLAGS="-O1 -g -std=c11 -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer" \
	        LDFLAGS="-fsanitize=address,undefined" all

clean:
	rm -rf build bench_data
