OS := $(shell uname)

ROOT_DIR = ../..
INCLUDE_DIR = $(ROOT_DIR)/include
BUILD_DIR = $(CURDIR)/build
LIB_DIR = $(BUILD_DIR)/debug

INCS = -I$(INCLUDE_DIR)
CFLAGS = -I.  -Werror -fsanitize=address -g
#-pedantic -Wall
ifeq ($(OS), Darwin)
CFLAGS += -framework Security
endif

LIBCRYPTO_DIR = $(dir $(shell find ${BUILD_DIR} -name libcrypto.a))
LIBSSL_DIR = $(dir $(shell find ${BUILD_DIR} -name libssl.a))

LDFLAGS = -L$(LIBCRYPTO_DIR) -L$(LIBSSL_DIR) -L$(LIB_DIR)

LIBS = $(LIB_DIR)/libquiche.a -lev -ldl -pthread -lm -lstdc++

all: client server

client: client.c $(INCLUDE_DIR)/quiche.h $(LIB_DIR)/libquiche.a
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@ $(INCS) $(LIBS)

server: server.c $(INCLUDE_DIR)/quiche.h $(LIB_DIR)/libquiche.a
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@ $(INCS) $(LIBS)

$(LIB_DIR)/libquiche.a: $(shell find $(SOURCE_DIR) -type f -name '*.rs')
	cd $(ROOT_DIR) && cargo build --target-dir $(BUILD_DIR)

clean:
	@$(RM) -rf client server build/