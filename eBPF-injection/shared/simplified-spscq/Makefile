CC=g++
#CC=clang++
PROGS=spscq
CFLAGS=-Wall -Werror -g
CXXFLAGS=$(CFLAGS) -std=c++11
CFLAGS+=-O2
LDFLAGS=-lpthread -std=c++11 -g

all: $(PROGS)

spscq: spscq.o mlib.o -lpthread 
mlib.o: mlib.h
spscq.o: spscq.h


clean:
	-rm -rf *.o $(PROGS)

format:
	clang-format -i -style=file $(shell git ls-files *.c *.h *.cpp *.hpp)
