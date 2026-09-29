CC ?= cc
PKG_CONFIG ?= pkg-config
OUT ?= $(abspath ../../../../../out/pc-logs/dbus-broker/build)
CFLAGS ?= -O2 -g
CPPFLAGS += -D_GNU_SOURCE
CFLAGS += -std=c11 -Wall -Wextra -Werror $(shell $(PKG_CONFIG) --cflags gio-2.0)
LDLIBS += $(shell $(PKG_CONFIG) --libs gio-2.0)

all: $(OUT)/matonos-dbus-broker

test: $(OUT)/broker-test

$(OUT)/matonos-dbus-broker: $(OUT)/main.o $(OUT)/broker.o $(OUT)/example.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/%.o: %.c broker.h | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OUT)/broker-test: test-client.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -rf $(OUT)

$(OUT):
	mkdir -p $@

.PHONY: all test clean
