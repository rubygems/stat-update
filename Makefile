
stat-update: stat-update.c \
		vendor/libebb/libebb.a \
		vendor/hiredis/libhiredis.a \
		vendor/libev/.libs/libev.a
	gcc -Ivendor/libebb -Ivendor/libev -Ivendor/hiredis  -c -o stat-update.o -O0 -ggdb stat-update.c
	gcc -o stat-update vendor/libebb/libebb.a vendor/libev/.libs/libev.a vendor/hiredis/libhiredis.a -O3 stat-update.o

clean:
	rm stat-update *.o

vendor/libebb/libebb.a:
	cp libebb-config.mk vendor/libebb/config.mk
	cd vendor/libebb; make libebb.a

vendor/hiredis/libhiredis.a:
	cd vendor/hiredis; make

vendor/libev/.libs/libev.a:
	cd vendor/libev; ./configure --disable-shared && make
