CC = gcc
CFLAGS = -g
CPPFLAGS = -std=gnu99 -Wall -pedantic -Wextra

OS = $(shell uname -o)

ifeq ($(OS), Msys) # the example server program is not compatible with Windows since it uses pthread.h
	VERSION = windows
	target = example-client-$(VERSION)
endif
ifeq ($(OS), GNU/Linux)
	VERSION = linux
	target = example-server-$(VERSION) example-client-$(VERSION)
endif

all: ./build/bin/$(VERSION) $(target)

#--- build dirs ---------------------------------------------------------------

./build/bin/$(VERSION):
	mkdir -p ./build/obj/$(VERSION)
	mkdir -p ./build/lib/$(VERSION)
	mkdir -p ./build/bin/$(VERSION)

#--- libNet -------------------------------------------------------------------

./build/lib/$(VERSION)/libNet.a: ./build/obj/$(VERSION)/net.o
	ar -r ./build/lib/$(VERSION)/libNet.a ./build/obj/$(VERSION)/net.o

./build/obj/$(VERSION)/net.o: ./lib/net_$(VERSION).c ./lib/net.h ./build/bin/$(VERSION)
	$(CC) -o ./build/obj/$(VERSION)/net.o -c $(CFLAGS) $(CPPFLAGS) ./lib/net_$(VERSION).c

#--- example server -----------------------------------------------------------

example-server-$(VERSION): ./build/bin/$(VERSION)/example-server
	ln -sf ./build/bin/$(VERSION)/example-server example-server-$(VERSION)

./build/bin/$(VERSION)/example-server: ./build/obj/$(VERSION)/example-server.o ./build/lib/$(VERSION)/libNet.a
	$(CC) $(CFLAGS) $(CPPFLAGS) -o ./build/bin/$(VERSION)/example-server ./build/obj/$(VERSION)/example-server.o -L./build/lib/$(VERSION) -lNet

./build/obj/$(VERSION)/example-server.o: example-server.c ./lib/net.h ./build/bin/$(VERSION)
	$(CC) -o ./build/obj/$(VERSION)/example-server.o -c $(CFLAGS) $(CPPFLAGS) example-server.c -I./lib

#--- example client -----------------------------------------------------------

example-client-$(VERSION): ./build/bin/$(VERSION)/example-client
	ln -sf ./build/bin/$(VERSION)/example-client example-client-$(VERSION)

./build/bin/$(VERSION)/example-client: ./build/obj/$(VERSION)/example-client.o ./build/lib/$(VERSION)/libNet.a
ifeq ($(VERSION), windows)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o ./build/bin/$(VERSION)/example-client ./build/obj/$(VERSION)/example-client.o -L./build/lib/$(VERSION) -lNet -lws2_32
else
	$(CC) $(CFLAGS) $(CPPFLAGS) -o ./build/bin/$(VERSION)/example-client ./build/obj/$(VERSION)/example-client.o -L./build/lib/$(VERSION) -lNet
endif

./build/obj/$(VERSION)/example-client.o: example-client.c ./lib/net.h ./build/bin/$(VERSION)
	$(CC) -o ./build/obj/$(VERSION)/example-client.o -c $(CFLAGS) $(CPPFLAGS) example-client.c -I./lib

#--- clean --------------------------------------------------------------------

clean:
	rm -rf ./build
	rm -f example-server-$(VERSION)
	rm -f example-client-$(VERSION)
