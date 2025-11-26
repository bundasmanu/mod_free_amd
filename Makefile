MODNAME = mod_free_amd.so
MODOBJ = mod_free_amd.o
MODDIR = /opt/freeswitch/mod
MODCFLAGS = -Wall -Werror
# MODLDFLAGS = -lssl

CC = gcc
CFLAGS = -fPIC -g -ggdb `pkg-config --cflags freeswitch` $(MODCFLAGS)
LDFLAGS = `pkg-config --libs freeswitch` $(MODLDFLAGS)

.PHONY: all
all: $(MODNAME)

$(MODNAME): $(MODOBJ)
	@$(CC) -shared -o $@ $(MODOBJ) $(LDFLAGS)

.c.o: $<
	@$(CC) $(CFLAGS) -o $@ -c $<

.PHONY: clean
clean:
	rm -f $(MODNAME) $(MODOBJ)

.PHONY: install
install: $(MODNAME)
	install -d $(DESTDIR)$(MODDIR)
	install $(MODNAME) $(DESTDIR)$(MODDIR)
