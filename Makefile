EXECUTABLE=dim-bsp64.exe
DEBUGNAME=dim-bsp64-debug.exe
 
CC="C:\msys64\mingw32\bin\gcc.exe"
LDFLAGS=-lmingw32

CFLAGS = -O2 -Wall -Wno-unknown-pragmas
 
src = $(wildcard *.c)
obj = $(src:.c=.o)
 
all: myprog
 
myprog: $(obj)
	$(CC) -o $(EXECUTABLE) $^ $(LDFLAGS)
	cp $(EXECUTABLE) ./bin/$(EXECUTABLE)
	cp $(EXECUTABLE) ./bin/$(DEBUGNAME)
	strip -s ./bin/$(EXECUTABLE)
	rm -rf $(EXECUTABLE)
 
.PHONY: clean
clean:
	rm $(obj) ./bin/$(EXECUTABLE) ./bin/$(DEBUGNAME)