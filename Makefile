CC := x86_64-w64-mingw32-gcc
CXX := x86_64-w64-mingw32-g++
CFLAGS := -O2 -Wall -Wextra -Wno-cast-function-type
OUT := build

all: $(OUT)/sppc.dll $(OUT)/ole32.dll $(OUT)/uiautomationcore.dll $(OUT)/d2d1.dll $(OUT)/xmllite.dll $(OUT)/qmgr.dll $(OUT)/wintrust.dll $(OUT)/bcryptprimitives.dll $(OUT)/resprotocol.dll $(OUT)/tar.exe $(OUT)/System.Management.Automation.dll

$(OUT)/sppc.dll: shims/sppc/sppc.c shims/sppc/sppc.def
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -shared -o $@ shims/sppc/sppc.c shims/sppc/sppc.def -nostdlib -lkernel32 -luser32 -Wl,--entry=DllMain

$(OUT)/ole32.dll: shims/ole32/ole32.c shims/ole32/importfix.c shims/ole32/ole32.def
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -shared -o $@ shims/ole32/ole32.c shims/ole32/importfix.c shims/ole32/ole32.def -nostdlib -lkernel32 -luser32 -lntdll -Wl,--entry=DllMain

$(OUT)/uiautomationcore.dll: shims/uiautomationcore/uiautomationcore.c shims/uiautomationcore/uiautomationcore.def
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -shared -o $@ shims/uiautomationcore/uiautomationcore.c shims/uiautomationcore/uiautomationcore.def -nostdlib -lkernel32 -lole32 -luuid -Wl,--entry=DllMain

$(OUT)/d2d1.dll: shims/d2d1/d2d1.cpp shims/d2d1/d2d1.def
	mkdir -p $(OUT)
	$(CXX) -O2 -Wall -Wextra -fno-exceptions -fno-rtti -shared -static -o $@ shims/d2d1/d2d1.cpp shims/d2d1/d2d1.def -lkernel32 -luser32 -lgdi32 -Wl,--entry=DllMain

$(OUT)/xmllite.dll: shims/xmllite/xmllite.c shims/xmllite/xmllite.def
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -shared -o $@ shims/xmllite/xmllite.c shims/xmllite/xmllite.def -nostdlib -lkernel32 -Wl,--entry=DllMain

$(OUT)/qmgr.dll: shims/qmgr/qmgr.cpp shims/qmgr/qmgr.def
	mkdir -p $(OUT)
	$(CXX) -O2 -Wall -Wextra -Wno-cast-function-type -fno-exceptions -fno-rtti -shared -static -o $@ shims/qmgr/qmgr.cpp shims/qmgr/qmgr.def -lwinhttp -lole32 -luuid -ladvapi32 -lkernel32 -Wl,--entry=DllMain

$(OUT)/wintrust.dll: shims/wintrust/wintrust.c shims/wintrust/regload.c shims/wintrust/wintrust.def
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -shared -o $@ shims/wintrust/wintrust.c shims/wintrust/regload.c shims/wintrust/wintrust.def -nostdlib -lkernel32 -luser32 -ladvapi32 -lcrypt32 -lbcrypt -lntdll -Wl,--entry=DllMain

$(OUT)/bcryptprimitives.dll: shims/bcryptprimitives/bcryptprimitives.c shims/bcryptprimitives/bcryptprimitives.def
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -shared -o $@ shims/bcryptprimitives/bcryptprimitives.c shims/bcryptprimitives/bcryptprimitives.def -nostdlib -lkernel32 -Wl,--entry=DllMain

$(OUT)/resprotocol.dll: shims/resprotocol/resprotocol.c shims/resprotocol/resprotocol.def
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -fno-tree-loop-distribute-patterns -shared -o $@ shims/resprotocol/resprotocol.c shims/resprotocol/resprotocol.def -nostdlib -lkernel32 -lole32 -luuid -lntdll -Wl,--entry=DllMain

$(OUT)/tar.exe: shims/tar/tar.c shims/tar/miniz.c shims/tar/miniz.h
	mkdir -p $(OUT)
	$(CC) -O2 -Wall -Wextra -municode -static -o $@ shims/tar/tar.c shims/tar/miniz.c

# Needs the .NET SDK (dotnet).
$(OUT)/System.Management.Automation.dll: shims/sma/sma.csproj shims/sma/Host.cs shims/sma/Engine.cs shims/sma/msft.pub
	mkdir -p $(OUT)
	dotnet build shims/sma/sma.csproj -c Release -o $(OUT)/sma
	cp $(OUT)/sma/System.Management.Automation.dll $@

clean:
	rm -rf $(OUT)

.PHONY: all clean
