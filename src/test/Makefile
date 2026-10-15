CFLAGS := -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 -DREPRO_HOST_TEST -iquote ../../payload/include
REPRO := ../../payload/src/repro

all: test_scan test_write libsimcart.so

test_scan: test_scan.c $(REPRO)/repro_scan.c
	$(CC) $(CFLAGS) -o $@ $^

test_write: test_write.c sim_cart.c $(REPRO)/repro_scan.c $(REPRO)/repro_cart.c $(REPRO)/repro_image.c
	$(CC) $(CFLAGS) -o $@ $^

# Simulated cart for the emulator tests (emu_test.py, flasher_emu_test.py)
libsimcart.so: sim_cart.c sim_cart.h
	$(CC) -O2 -fPIC -shared -std=gnu11 -o $@ sim_cart.c

check: all
	./test_scan
	./test_write

# Emulator tests: the payload needs `make repro` at the top level first,
# the flasher needs `make` in ../flasher and a real Emerald dump.
emu: libsimcart.so
	python3 emu_test.py

flasher: libsimcart.so
	python3 flasher_emu_test.py $(DUMP)

clean:
	rm -f test_scan test_write libsimcart.so

.PHONY: all check emu flasher clean
