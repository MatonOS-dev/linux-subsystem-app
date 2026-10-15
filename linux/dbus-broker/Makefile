CC ?= cc
PKG_CONFIG ?= pkg-config
OUT ?= $(abspath ../../../../../out/pc-logs/dbus-broker/build)
CFLAGS ?= -O2 -g
CPPFLAGS += -D_GNU_SOURCE
CFLAGS += -std=c11 -Wall -Wextra -Werror $(shell $(PKG_CONFIG) --cflags gio-2.0)
LDLIBS += $(shell $(PKG_CONFIG) --libs gio-2.0)

all: test

test: $(OUT)/inhibit-test $(OUT)/portal-wire-test $(OUT)/policy-wildcard-test

$(OUT)/%.o: %.c broker.h | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(OUT)

$(OUT):
	mkdir -p $@

.PHONY: all test clean

$(OUT)/inhibit-test: inhibit-test.c broker.c broker.h $(OUT)/portals.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ inhibit-test.c $(OUT)/portals.o $(LDLIBS)

check-java-portals:
	../subsystem/host/test-portals.sh $(OUT)/java-portals

check-inhibit: $(OUT)/inhibit-test check-java-portals
	MATON_PORTAL_TEST_CLASSPATH=$(OUT)/java-portals $(OUT)/inhibit-test

.PHONY: check-inhibit

$(OUT)/portal-wire-test: portal-wire-test.c portals.c broker.h $(OUT)/broker.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ portal-wire-test.c $(OUT)/broker.o $(LDLIBS)

check-portal-wire: $(OUT)/portal-wire-test check-java-portals
	MATON_PORTAL_TEST_CLASSPATH=$(OUT)/java-portals $(OUT)/portal-wire-test

.PHONY: check-java-portals check-portal-wire

$(OUT)/policy-wildcard-test: policy-wildcard-test.c broker.c broker.h $(OUT)/portals.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ policy-wildcard-test.c $(OUT)/portals.o $(LDLIBS)

check-policy-wildcard: $(OUT)/policy-wildcard-test
	$<
