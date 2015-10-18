sdtest: sdtest.c
	gcc -g -D_USE_GNU -O0 $^ -o $@

clean:
	rm -f sdtest

deps:
	gcc -g -MD sdtest.c

