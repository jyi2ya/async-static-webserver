server: main.c
	gcc -O2 -g main.c -o server

clean:
	rm -f server
