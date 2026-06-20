CC      = cc
CFLAGS  = -O2 -Wall -Wextra -std=c99
U       = utility-apps
PREFIX  = /usr/local
BINDIR  = $(DESTDIR)$(PREFIX)/bin

.PHONY: all clean install uninstall

all: zzwm zzwm-run zzwm-bar zzwm-help

zzwm: zzwm.c config.h appearance.h
	$(CC) $(CFLAGS) -o $@ $< -lX11 -lXrender -lXcomposite -lXdamage

zzwm-run: $(U)/runner.c appearance.h
	$(CC) $(CFLAGS) -o $@ $< -lX11

zzwm-bar: $(U)/statusbar.c appearance.h
	$(CC) $(CFLAGS) -o $@ $< -lX11

zzwm-help: $(U)/help.c appearance.h config.h
	$(CC) $(CFLAGS) -o $@ $< -lX11

clean:
	rm -f zzwm zzwm-run zzwm-bar zzwm-help

install: all
	install -d $(BINDIR)
	install -m755 zzwm zzwm-run zzwm-bar zzwm-help $(BINDIR)

uninstall:
	rm -f $(BINDIR)/zzwm $(BINDIR)/zzwm-run $(BINDIR)/zzwm-bar $(BINDIR)/zzwm-help
