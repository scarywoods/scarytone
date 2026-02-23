CC = gcc
CFLAGS = -Wall -O2 `pkg-config --cflags libavformat libavcodec libswresample libavutil sdl2`
LIBS = `pkg-config --libs libavformat libavcodec libswresample libavutil sdl2`

all: audioplayer

audioplayer: main.c
	$(CC) main.c -o audioplayer $(CFLAGS) $(LIBS)

clean:
	rm -f audioplayer
