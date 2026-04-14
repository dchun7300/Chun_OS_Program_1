CC = gcc
CFLAGS = -Wall -g -fPIC

# all object files
OBJS = malloc.o

# default target
all: libmalloc.a libmalloc.so

# static library
libmalloc.a: $(OBJS)
	ar rcs libmalloc.a $(OBJS)

# shared library
libmalloc.so: $(OBJS)
	$(CC) $(CFLAGS) -shared -o libmalloc.so $(OBJS)

# individual object rules
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# clean rule
clean:
	rm -f *.o *.a *.so *~

malloc: all
	@echo "Built malloc libraries."
.PHONY: all clean malloc
