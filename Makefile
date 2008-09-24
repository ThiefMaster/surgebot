BIN = surgebot
-include modules.build

LIBS = -lssl -ldl
CFLAGS = -pipe -Werror -Wall -g -fPIC
LDFLAGS = -Wl,--export-dynamic

SRC = $(wildcard *.c)
OBJ = $(patsubst %.c,.tmp/%.o,$(SRC))
DEP = $(patsubst %.c,.tmp/%.d,$(SRC))

.PHONY: all clean distclean

all: $(DEP) $(BIN) $(MODULES)

clean:
	@echo "   CLEAN"
	@rm -f $(BIN) .tmp/*.d .tmp/*.o modules/*.so module-config.h
	@for i in $(MODULES); do make -s -f Makefile.module MODULE=$$i clean ; done

# rule for creating final binary
$(BIN): $(OBJ)
	@echo "   LD        $@"
	@$(CC) $(LDFLAGS) $(LIBS) $(OBJ) -o $(BIN)

# rule for creating object files
$(OBJ) : .tmp/%.o : %.c
	@echo "   CC        $(<:.c=.o)"
	@$(CC) $(CFLAGS) -std=gnu99 -MMD -MF .tmp/$(<:.c=.d) -MT $@ -o $@ -c $<

# rule for creating dependency files
$(DEP) : .tmp/%.d : %.c module-config.h
	@mkdir -p .tmp
	@echo "   DEP       $(<:.c=.d)"
	@$(CC) $(CFLAGS) -std=gnu99 -MM -MT $(@:.d=.o) $< > $@

module-config.h: modules.build
	@echo "   MODCONF"
	@rm -f module-config.h
	@echo "#ifndef MODULE_CONFIG_H" >> module-config.h
	@echo -e "#define MODULE_CONFIG_H\n" >> module-config.h
	@for i in $(MODULES); do echo "#define WITH_MODULE_$$i" >> module-config.h ; done
	@echo -e "\n#endif" >> module-config.h

# include dependency files
ifneq ($(MAKECMDGOALS),clean)
-include $(DEP)
endif

# build modules
$(MODULES):
	@make -s -f Makefile.module MODULE=$@
