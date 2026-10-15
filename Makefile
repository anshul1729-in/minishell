CC = gcc
CFLAGS = -Wall -Wextra -g

build:
	$(CC) $(CFLAGS) -o minishell main.c

run: build
	./minishell

clean:
	rm -f minishell