SHELL := /bin/sh
OBJS := main.o cJSON.o
BINARY := rac
CFLAGS += -Wall -O2

.PHONY: all
all: build

.PHONY: build
build: $(OBJS)
	$(CC) $(CFLAGS) -o $(BINARY) $(OBJS) -luv -lpq -lllhttp

.PHONY: clean
clean:
	$(RM) $(OBJS) $(BINARY)

%.o : %.c
	$(CC) -c $(CFLAGS) $< -o $@
