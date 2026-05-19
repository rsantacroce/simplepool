test_store_bin = build/test_store
$(test_store_bin): tests/test_store.c src/store.c src/log.c
	mkdir -p build
	$(CC) $(CFLAGS) -Isrc -o $(test_store_bin) tests/test_store.c src/store.c src/log.c -lsqlite3 -lpthread
test_store: $(test_store_bin)
	./$(test_store_bin)
