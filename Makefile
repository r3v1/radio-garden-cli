CC = g++
CFLAGS = -std=c++17 -Wall -Wextra -I/usr/include
LIBS = -lcurl -lncurses -lsystemd -pthread
SOURCES = src/main.cpp src/radio.cpp src/requests.cpp src/paths.cpp \
          src/state.cpp src/index.cpp src/player.cpp src/mpris.cpp src/tui.cpp
EXECUTABLE = radio

all: $(EXECUTABLE)

$(EXECUTABLE): $(SOURCES)
	$(CC) $(CFLAGS) $(SOURCES) -o $(EXECUTABLE) $(LIBS)

clean:
	rm -f $(EXECUTABLE)

.PHONY: all clean
