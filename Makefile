CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS =

all: launcher server pseudoAI_model window

launcher: launcher.c
	$(CC) $(CFLAGS) launcher.c -o launcher -pthread

server: server.c
	$(CC) $(CFLAGS) server.c -o server

pseudoAI_model: pseudoAI_model.c
	$(CC) $(CFLAGS) pseudoAI_model.c -o pseudoAI_model

window: window.c
	$(CC) $(CFLAGS) window.c -o window -lX11

clean:
	rm -f launcher server window pseudoAI_model *.o server.log results.log window_*.txt window_*.queue

.PHONY: all clean