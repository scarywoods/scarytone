CC = gcc
CFLAGS = -Wall -O2 `pkg-config --cflags libavformat libavcodec libswresample libavutil sdl2`
LIBS = `pkg-config --libs libavformat libavcodec libswresample libavutil sdl2`

all: scarytone

scarytone: main.c
	$(CC) main.c -o scarytone $(CFLAGS) $(LIBS)

clean:
	rm -f scarytone
