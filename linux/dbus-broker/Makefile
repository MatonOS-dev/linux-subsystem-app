CC ?= cc
PKG_CONFIG ?= pkg-config
OUT ?= $(abspath ../../../../../out/pc-logs/dbus-broker/build)
CFLAGS ?= -O2 -g
CPPFLAGS += -D_GNU_SOURCE
CFLAGS += -std=c11 -Wall -Wextra -Werror $(shell $(PKG_CONFIG) --cflags gio-2.0)
LDLIBS += $(shell $(PKG_CONFIG) --libs gio-2.0)

all: $(OUT)/matonos-dbus-broker

test: $(OUT)/broker-test $(OUT)/inhibit-test

$(OUT)/matonos-dbus-broker: $(OUT)/main.o $(OUT)/broker.o $(OUT)/example.o $(OUT)/portals.o
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

$(OUT)/inhibit-test: inhibit-test.c broker.c broker.h $(OUT)/portals.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ inhibit-test.c $(OUT)/portals.o $(LDLIBS)

check-inhibit: $(OUT)/inhibit-test
	$<

.PHONY: check-inhibit
