sources := main.c


all: warplan warplan-d

warplan: $(sources)
	gcc -std=c99 -g -Wall -O3 -o $@ -D_POSIX_C_SOURCE=200809L $^

warplan-d: $(sources)
	gcc -std=c99 -g -O0 -Wall -o $@ -D_POSIX_C_SOURCE=200809L $^

clean:
	rm -f warplan warplan-d
