CC      = cc
CFLAGS  = -O2 -Wall -Wextra -std=c99
LDFLAGS = -lX11 -lXrender -lXcomposite -lXdamage

zwm: zwm.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f zwm
