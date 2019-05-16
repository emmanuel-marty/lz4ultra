CC=clang
CFLAGS=-O3 -fomit-frame-pointer -Isrc/libdivsufsort/include -Isrc/xxhash -Isrc -DHAVE_CONFIG_H
OBJDIR=obj
LDFLAGS=
STRIP=strip

$(OBJDIR)/%.o: src/../%.c
	@mkdir -p '$(@D)'
	$(CC) $(CFLAGS) -c $< -o $@

APP := lz4ultra

OBJS := $(OBJDIR)/src/lz4ultra.o
OBJS += $(OBJDIR)/src/lib.o
OBJS += $(OBJDIR)/src/inmem.o
OBJS += $(OBJDIR)/src/stream.o
OBJS += $(OBJDIR)/src/frame.o
OBJS += $(OBJDIR)/src/matchfinder.o
OBJS += $(OBJDIR)/src/shrink.o
OBJS += $(OBJDIR)/src/expand.o
OBJS += $(OBJDIR)/src/libdivsufsort/lib/divsufsort.o
OBJS += $(OBJDIR)/src/libdivsufsort/lib/sssort.o
OBJS += $(OBJDIR)/src/libdivsufsort/lib/trsort.o
OBJS += $(OBJDIR)/src/libdivsufsort/lib/utils.o
OBJS += $(OBJDIR)/src/xxhash/xxhash.o

all: $(APP)

$(APP): $(OBJS)
	@mkdir -p ../../bin/posix
	$(CC) $^ $(LDFLAGS) -o $(APP)
	$(STRIP) $(APP)

clean:
	@rm -rf $(APP) $(OBJDIR)

