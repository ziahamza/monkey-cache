all: monkey-cache.so
include ../Make.common

CC	= @echo "  CC   $(_PATH)/$@"; gcc
CC_QUIET= @echo -n; gcc
CFLAGS	=   -std=gnu99 -Wall -Wextra -fvisibility=hidden -O2
LDFLAGS =
DEFS    =
CACHE_OBJECTS = cache.o

-include $(CACHE_OBJECTS:.o=.d)

monkey-cache.so: $(CACHE_OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(DEFS) -shared -o $@ $^ -lc
