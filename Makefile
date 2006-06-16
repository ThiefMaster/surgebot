MODULES = simple simpledep simpledepdep
BIN = surgebot
CFLAGS = -pipe -Wall -g -fPIC

OBJ = $(patsubst %.c,%.o,$(wildcard *.c))

.PHONY: all all-before all-after clean clean-custom

all: all-before $(BIN) $(MODULES) all-after

clean: clean-custom
	rm -f $(OBJ) $(BIN)
	./clean-modules $(MODULES)

$(BIN): $(OBJ)
	@echo "[LD] $@"
	@gcc $(CFLAGS) -lssl -ldl -Wl,--export-dynamic $(OBJ) -o $(BIN)

$(MODULES):
	@cd modules/$@; make

.c.o:
	@echo "[CC] $@"
	@gcc ${CFLAGS} -o $@ -c $^
