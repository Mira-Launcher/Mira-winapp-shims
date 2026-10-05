CC := x86_64-w64-mingw32-gcc
OUT := build

all:
	mkdir -p $(OUT)

clean:
	rm -rf $(OUT)

.PHONY: all clean
