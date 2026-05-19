build/test_bitcoind: tests/test_bitcoind.c src/bitcoind.c src/cjson/cJSON.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o build/test_bitcoind $^ -lcurl
