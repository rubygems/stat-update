
stat-update: stat-update.c \
		vendor/libebb.a \
		vendor/libhiredis.a \
		vendor/libev.a
	gcc -Ivendor/libebb -Ivendor/libev -Ivendor/hiredis  -c -o stat-update.o -O0 -ggdb stat-update.c
	gcc -o stat-update stat-update.o vendor/libebb.a vendor/libev.a vendor/libhiredis.a -lm

clean:
	rm stat-update *.o

vendor/libebb.a:
	cp libebb-config.mk vendor/libebb/config.mk
	cd vendor/libebb; make libebb.a && mv libebb.a .. && git checkout config.mk

vendor/libhiredis.a:
	cd vendor/hiredis; make && cp libhiredis.a ..

vendor/libev.a:
	cd vendor/libev; ./configure --disable-shared && make && cp .libs/libev.a ..
