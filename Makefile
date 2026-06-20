CC      = cc
CFLAGS  = -O2 -Wall -Wextra -std=c99
U       = utility-apps

all: zzwm zzwm-run zzwm-bar zzwm-help

zzwm: zzwm.c config.h appearance.h
	$(CC) $(CFLAGS) -o $@ $< -lX11 -lXrender -lXcomposite -lXdamage -lXrandr

zzwm-run: $(U)/runner.c appearance.h
	$(CC) $(CFLAGS) -o $@ $< -lX11

zzwm-bar: $(U)/statusbar.c appearance.h
	$(CC) $(CFLAGS) -o $@ $< -lX11

zzwm-help: $(U)/help.c appearance.h config.h
	$(CC) $(CFLAGS) -o $@ $< -lX11

clean:
	rm -f zzwm zzwm-run zzwm-bar zzwm-help
