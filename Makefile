src = $(wildcard *.c) $(wildcard LLMProxy/c/src/*.c) 
obj = $(src:.c=.o)
CC = gcc
CFLAGS = -Wall -g -I./LLMProxy/c/include -I./LLMProxy/c/src -Ijansson/build/include -I$(HOME)/.local/include

# Jansson static lib
JANSSON_LIB = jansson/build/lib/libjansson.a

LDFLAGS = \
    -L$(HOME)/.local/lib \
    $(JANSSON_LIB) \
    -lssl -lcrypto -lpthread -lnsl -lcurl -lgumbo \
    -Wl,-rpath,$(HOME)/.local/lib

proxy: $(obj)
	$(CC) -o $@ $^ $(LDFLAGS)

.PHONY: clean
clean:
	rm -f $(obj) proxy