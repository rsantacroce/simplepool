build/test_upstream: tests/test_upstream.c src/upstream.c \
                     src/log.c src/cjson/cJSON.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o build/test_upstream $^ -lpthread
