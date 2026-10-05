CC := x86_64-w64-mingw32-gcc
CFLAGS := -O2 -Wall -Wextra -Wno-cast-function-type
OUT := build

all: $(OUT)/sppc.dll $(OUT)/ole32.dll

$(OUT)/sppc.dll: shims/sppc/sppc.c shims/sppc/sppc.def
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -shared -o $@ shims/sppc/sppc.c shims/sppc/sppc.def -nostdlib -lkernel32 -luser32 -Wl,--entry=DllMain

$(OUT)/ole32.dll: shims/ole32/ole32.c shims/ole32/importfix.c shims/ole32/ole32.def
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -shared -o $@ shims/ole32/ole32.c shims/ole32/importfix.c shims/ole32/ole32.def -nostdlib -lkernel32 -lntdll -Wl,--entry=DllMain

clean:
	rm -rf $(OUT)

.PHONY: all clean
