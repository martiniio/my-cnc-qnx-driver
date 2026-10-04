ARTIFACTS = MyDriver cnc_run

#Build architecture/variant string, possible values: x86_64, armv7le, etc...
PLATFORM ?= x86_64

#Build profile, possible values: release, debug, profile, coverage
BUILD_PROFILE ?= debug

CONFIG_NAME ?= $(PLATFORM)-$(BUILD_PROFILE)
OUTPUT_DIR = build/$(CONFIG_NAME)

#Compiler definitions

CC = qcc -Vgcc_nto$(PLATFORM)
LD = $(CC)

#User defined include/preprocessor flags and libraries
INCLUDES += -Isrc
CCFLAGS  += -D_QNX_SOURCE
LIBS     += -lsocket

#Compiler flags for build profiles
CCFLAGS_release += -O2
CCFLAGS_debug += -g -O0 -fno-builtin

#Generic compiler flags (which include build type flags)
CCFLAGS_all += -Wall -fmessage-length=0
CCFLAGS_all += $(CCFLAGS_$(BUILD_PROFILE))
LDFLAGS_all += $(LDFLAGS_$(BUILD_PROFILE))
LIBS_all += $(LIBS_$(BUILD_PROFILE))
DEPS = -Wp,-MMD,$(@:%.o=%.d),-MT,$@

#Two binaries. The driver links open62541; the example tool does not.
DRIVER_SRCS = src/MyDriver.c src/opcua_client.c src/open62541.c
TOOL_SRCS   = src/cnc_run.c

DRIVER_OBJS = $(addprefix $(OUTPUT_DIR)/,$(addsuffix .o,$(basename $(DRIVER_SRCS))))
TOOL_OBJS   = $(addprefix $(OUTPUT_DIR)/,$(addsuffix .o,$(basename $(TOOL_SRCS))))

DRIVER_BIN = $(OUTPUT_DIR)/MyDriver
TOOL_BIN   = $(OUTPUT_DIR)/cnc_run

#Compiling rule
$(OUTPUT_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -c $(DEPS) -o $@ $(INCLUDES) $(CCFLAGS_all) $(CCFLAGS) $<

#Linking rules
$(DRIVER_BIN): $(DRIVER_OBJS)
	$(LD) -o $@ $(LDFLAGS_all) $(LDFLAGS) $^ $(LIBS_all) $(LIBS)

$(TOOL_BIN): $(TOOL_OBJS)
	$(LD) -o $@ $(LDFLAGS_all) $(LDFLAGS) $^ $(LIBS_all) $(LIBS)

all: $(DRIVER_BIN) $(TOOL_BIN)

clean:
	rm -fr $(OUTPUT_DIR)

rebuild: clean all

-include $(DRIVER_OBJS:%.o=%.d) $(TOOL_OBJS:%.o=%.d)