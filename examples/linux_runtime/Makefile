CC ?= cc
CPPFLAGS += -I../../include -I../appliance_manager -I../../ports/linux
CFLAGS ?= -O2 -g
CFLAGS += -std=c99 -Wall -Wextra -Werror -pedantic -pthread
LDLIBS += -pthread
TARGET ?= ../../build/linux_runtime
SOURCES := main.c $(wildcard ../../src/*.c ../appliance_manager/*.c ../../ports/linux/*.c)
HEADERS := $(wildcard ../../include/*.h ../appliance_manager/*.h ../../ports/linux/*.h)

.PHONY: all
all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS) Makefile
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@
