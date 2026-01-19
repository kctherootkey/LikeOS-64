CC = gcc
AR = ar
CFLAGS = -Wall -Wextra -O2 -nostdlib -fno-builtin -fno-stack-protector \
         -mno-red-zone -ffreestanding -I./include

# Source files
MALLOC_SRC = src/malloc/malloc.c
STDIO_SRC = src/stdio/stdio.c
STRING_SRC = src/string/string.c
STDLIB_SRC = src/stdlib/stdlib.c
CTYPE_SRC = src/ctype/ctype.c
SYSCALLS_SRC = src/syscalls/unistd.c src/syscalls/mman.c
CRT0_SRC = src/crt0.S

# Object files
MALLOC_OBJ = $(MALLOC_SRC:.c=.o)
STDIO_OBJ = $(STDIO_SRC:.c=.o)
STRING_OBJ = $(STRING_SRC:.c=.o)
STDLIB_OBJ = $(STDLIB_SRC:.c=.o)
CTYPE_OBJ = $(CTYPE_SRC:.c=.o)
SYSCALLS_OBJ = $(SYSCALLS_SRC:.c=.o)
CRT0_OBJ = $(CRT0_SRC:.S=.o)

ALL_OBJ = $(MALLOC_OBJ) $(STDIO_OBJ) $(STRING_OBJ) $(STDLIB_OBJ) \
          $(CTYPE_OBJ) $(SYSCALLS_OBJ)

# Output
LIBC = libc.a
CRT0 = crt0.o

all: $(LIBC) $(CRT0)

$(LIBC): $(ALL_OBJ)
	$(AR) rcs $@ $^

$(CRT0): $(CRT0_OBJ)
	cp $< $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(ALL_OBJ) $(CRT0_OBJ) $(LIBC) $(CRT0)

.PHONY: all clean
