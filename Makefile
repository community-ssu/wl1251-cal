wl1251-cal: wl1251-cal.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o wl1251-cal wl1251-cal.c -lcal -lnl $(shell pkg-config --cflags --libs dbus-1)

install:
	install -d $(DESTDIR)/etc/event.d
	install script/wl1251-cal $(DESTDIR)/etc/event.d
	install -d $(DESTDIR)/usr/bin
	install wl1251-cal $(DESTDIR)/usr/bin

clean:
	rm wl1251-cal
