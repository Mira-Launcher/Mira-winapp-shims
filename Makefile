CC := x86_64-w64-mingw32-gcc
CFLAGS := -O2 -Wall -Wextra -Wno-cast-function-type
OUT := build

all: $(OUT)/sppc.dll

$(OUT)/sppc.dll: shims/sppc/sppc.c shims/sppc/sppc.def
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -shared -o $@ shims/sppc/sppc.c shims/sppc/sppc.def -nostdlib -lkernel32 -luser32 -Wl,--entry=DllMain

clean:
	rm -rf $(OUT)

.PHONY: all clean
