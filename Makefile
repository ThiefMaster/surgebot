BIN = surgebot
-include modules.build

LIBS = -lssl -ldl
CFLAGS = -pipe -Wall -g -fPIC
LDFLAGS = -Wl,--export-dynamic

SRC = $(wildcard *.c)
OBJ = $(patsubst %.c,.tmp/%.o,$(SRC))
DEP = $(patsubst %.c,.tmp/%.d,$(SRC))

.PHONY: all clean distclean

all: $(DEP) $(BIN) $(MODULES)

clean:
	@echo "   CLEAN"
	@rm -f $(BIN) .tmp/*.d .tmp/*.o modules/*.so
	@for i in $(MODULES); do make -s -f Makefile.module MODULE=$$i clean ; done

# rule for creating final binary
$(BIN): $(OBJ)
	@echo "   LD        $@"
	@$(CC) $(LDFLAGS) $(LIBS) $(OBJ) -o $(BIN)

# rule for creating object files
$(OBJ) : .tmp/%.o : %.c
	@echo "   CC        $(<:.c=.o)"
	@$(CC) $(CFLAGS) -MMD -MF .tmp/$(<:.c=.d) -MT $@ -o $@ -c $<

# rule for creating dependency files
$(DEP) : .tmp/%.d : %.c
	@mkdir -p .tmp
	@echo "   DEP       $(<:.c=.d)"
	@$(CC) $(CFLAGS) -MM -MT $(@:.d=.o) $< > $@

# include dependency files
ifneq ($(MAKECMDGOALS),clean)
-include $(DEP)
endif

# build modules
$(MODULES):
	@make -s -f Makefile.module MODULE=$@
