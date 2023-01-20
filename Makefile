CC = g++
CFLAGS = -std=c++11 -I/usr/include
LIBS = -lcurl 
SOURCES = src/main.cpp src/radio.cpp src/requests.cpp  
EXECUTABLE = radio

all: $(SOURCES)
	$(CC) $(CFLAGS) $(SOURCES) -o $(EXECUTABLE) $(LIBS)

clean:
	rm $(EXECUTABLE)
