BIN = surgebot
-include modules.build

LIBS = -lssl -ldl
CFLAGS = -pipe -Werror -Wall -Wextra -Wno-unused -g -fPIC
LDFLAGS = -Wl,--export-dynamic

SRC = $(wildcard *.c)
OBJ = $(patsubst %.c,.tmp/%.o,$(SRC))
DEP = $(patsubst %.c,.tmp/%.d,$(SRC))
TMPDIR = .tmp

.PHONY: all clean

all: $(TMPDIR) module-config.h $(BIN) $(MODULES)

clean:
ifdef NOCOLOR
	@printf "   CLEAN\n"
else
	@printf "   \033[38;5;154mCLEAN\033[0m\n"
endif
	@rm -f $(BIN) $(TMPDIR)/*.d $(TMPDIR)/*.o modules/*.so module-config.h
	@for i in $(MODULES); do make -s -f Makefile.module MODULE=$$i clean ; done

# rule for creating final binary
$(BIN): $(OBJ)
ifdef NOCOLOR
	@printf "   LD        $@\n"
else
	@printf "   \033[38;5;69mLD\033[0m        $@\n"
endif
	@$(CC) $(LDFLAGS) $(LIBS) $(OBJ) -o $(BIN)

# rule for creating object files
$(OBJ) : $(TMPDIR)/%.o : %.c
ifdef NOCOLOR
	@printf "   CC        $(<:.c=.o)\n"
else
	@printf "   \033[38;5;33mCC\033[0m        $(<:.c=.o)\n"
endif
	@$(CC) $(CFLAGS) -std=gnu99 -MMD -MF $(TMPDIR)/$(<:.c=.d) -MT $@ -o $@ -c $<
	@cp -f $(TMPDIR)/$*.d $(TMPDIR)/$*.d.tmp
	@sed -e 's/.*://' -e 's/\\$$//' < $(TMPDIR)/$*.d.tmp | fmt -1 | sed -e 's/^ *//' -e 's/$$/:/' >> $(TMPDIR)/$*.d
	@rm -f $(TMPDIR)/$*.d.tmp

module-config.h: modules.build
ifdef NOCOLOR
	@printf "   MODCONF\n"
else
	@printf "   \033[38;5;208mMODCONF\033[0m\n"
endif
	@rm -f module-config.h
	@echo "#ifndef MODULE_CONFIG_H" >> module-config.h
	@echo "#define MODULE_CONFIG_H" >> module-config.h
	@for i in $(MODULES); do echo "#define WITH_MODULE_$$i" >> module-config.h ; done
	@echo "#endif" >> module-config.h

$(TMPDIR):
	@mkdir $(TMPDIR)

# include dependency files
ifneq ($(MAKECMDGOALS),clean)
-include $(DEP)
endif

# build modules
$(MODULES):
	@make -s -f Makefile.module MODULE=$@
