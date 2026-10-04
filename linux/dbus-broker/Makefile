CC ?= cc
PKG_CONFIG ?= pkg-config
OUT ?= $(abspath ../../../../../out/pc-logs/dbus-broker/build)
CFLAGS ?= -O2 -g
CPPFLAGS += -D_GNU_SOURCE
CFLAGS += -std=c11 -Wall -Wextra -Werror $(shell $(PKG_CONFIG) --cflags gio-2.0)
LDLIBS += $(shell $(PKG_CONFIG) --libs gio-2.0)

all: $(OUT)/matonos-dbus-broker

test: $(OUT)/broker-test $(OUT)/inhibit-test $(OUT)/portal-credentials-test $(OUT)/flatpak-version-test $(OUT)/native-bus-test

$(OUT)/matonos-dbus-broker: $(OUT)/main.o $(OUT)/broker.o $(OUT)/example.o $(OUT)/portals.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/%.o: %.c broker.h session-control.h flatpak-compat.h | $(OUT)
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

$(OUT)/portal-credentials-test: portal-credentials-test.c broker.c broker.h session-control.h flatpak-compat.h $(OUT)/portals.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ portal-credentials-test.c $(OUT)/portals.o $(LDLIBS)

check-portal-credentials: $(OUT)/portal-credentials-test
	$<

.PHONY: check-portal-credentials

$(OUT)/flatpak-version-test: flatpak-version-test.c flatpak-compat.h | $(OUT)
	$(CC) -std=c11 -Wall -Wextra -Werror -o $@ $<

check-flatpak-version: $(OUT)/flatpak-version-test
	$<

.PHONY: check-flatpak-version

$(OUT)/native-bus-test: native-bus-test.c broker.c broker.h session-control.h flatpak-compat.h $(OUT)/portals.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ native-bus-test.c $(OUT)/portals.o $(LDLIBS)

check-native-bus: $(OUT)/native-bus-test
	$<

.PHONY: check-native-bus
