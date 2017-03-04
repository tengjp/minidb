FILES = main.c readline.c myqsort.c
minidb:
	gcc -g $(FILES) -std=c99 -o minidb
