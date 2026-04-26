CXX      = g++

export PKG_CONFIG_PATH := /usr/lib/x86_64-linux-gnu/pkgconfig:/usr/lib64/pkgconfig:/usr/share/pkgconfig:$(PKG_CONFIG_PATH)

FUSE_INC := $(shell pkg-config --cflags fuse3 2>/dev/null || echo "-I/usr/include/fuse3")
FUSE_LIB := $(shell pkg-config --libs   fuse3 2>/dev/null || echo "-lfuse3 -lpthread")
CURL_LIB := $(shell curl-config --libs  2>/dev/null       || echo "-lcurl")

CXXFLAGS = -std=c++17 -Wall -Wextra -O2 \
           -D_FILE_OFFSET_BITS=64        \
           -DFUSE_USE_VERSION=35         \
           -Isrc/                        \
           $(FUSE_INC)

LDFLAGS  = $(FUSE_LIB) $(CURL_LIB)

SRCS = src/main.cpp src/tgfs.cpp src/telegram_client.cpp
OBJS = $(SRCS:.cpp=.o)

.PHONY: all clean

all: tgfs

tgfs: $(OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) tgfs