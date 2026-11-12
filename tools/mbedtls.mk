# Builds Mbed TLS's library into $(OUT)/libmbedtls.a with $(CC), $(AR) and $(MBED_CFLAGS).
# The Makefile calls this once the sources are in $(MBED_DIR), so the wildcard sees them.
SRC := $(wildcard $(MBED_DIR)/library/*.c)
OBJ := $(patsubst $(MBED_DIR)/library/%.c,$(OUT)/mbedtls/%.o,$(SRC))

$(OUT)/libmbedtls.a: $(OBJ)
	rm -f $@
	$(AR) rcs $@ $^

$(OUT)/mbedtls/%.o: $(MBED_DIR)/library/%.c
	@mkdir -p $(dir $@)
	$(CC) $(MBED_CFLAGS) -I$(MBED_DIR)/include -I$(MBED_DIR)/library -c -o $@ $<
