TARGET = battery_perc
OBJS = main.o patch_vsh.o patch_impose.o notice.o batlog.o

CFLAGS = -O2 -G0 -Wall -std=c99
ifeq ($(DEBUG), 1)
CFLAGS += -DDEBUG
endif
ifeq ($(BATLOG), 1)
CFLAGS += -DBATLOG
endif
ASFLAGS = $(CFLAGS)
LDFLAGS = -nostartfiles

BUILD_PRX = 1
PRX_EXPORTS = exports.exp

USE_KERNEL_LIBS = 1
USE_KERNEL_LIBC = 1

LIBS = -lpspsystemctrl_kernel -lpsppower_driver

EXTRA_CLEAN = ../$(TARGET).prx

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

all: ../$(TARGET).prx

../$(TARGET).prx: $(TARGET).prx
	cp $< $@
