TARGET	= udcd_uvc
OBJS	= src/main.o
LIBS	= -lSceSysmemForDriver_stub -lSceThreadmgrForDriver_stub \
	-lSceCpuForDriver_stub -lSceUdcdForDriver_stub \
	-lSceDisplayForDriver_stub -lSceIftuForDriver_stub \
	-ltaihenForKernel_stub -lSceSysclibForDriver_stub

ifeq ($(DEBUG), 1)
	OBJS	+= debug/log.o debug/draw.o debug/console.o debug/font_data.o
	CFLAGS	+= -DDEBUG -Idebug
	LIBS	+= -lSceIofilemgrForDriver_stub
endif

ifeq ($(DIAG), 1)
	OBJS	+= src/diag.o
	CFLAGS	+= -DDIAG
	LIBS	+= -lSceIofilemgrForDriver_stub
endif

# Diagnostic only: ignore the host-negotiated frame index and always send
# the native 960x544 mode. Do not ship a build with this enabled.
ifeq ($(FORCE_NATIVE_RES), 1)
	CFLAGS	+= -DFORCE_NATIVE_RES
endif

ifeq ($(DISPLAY_OFF_OLED), 1)
	CFLAGS	+= -DDISPLAY_OFF_OLED
	LIBS	+= -lSceOledForDriver_stub
endif

ifeq ($(DISPLAY_OFF_LCD), 1)
	CFLAGS	+= -DDISPLAY_OFF_LCD
	LIBS	+= -lSceLcdForDriver_stub
endif

PREFIX	= arm-vita-eabi
CC	= $(PREFIX)-gcc
CFLAGS	+= -std=gnu17 -Wl,-q -Wall -O2 -nostartfiles -mcpu=cortex-a9 -mthumb-interwork -Iinclude
DEPS	= $(OBJS:.o=.d)

all: $(TARGET).skprx

%.skprx: %.velf
	vita-make-fself -c $< $@

%.velf: %.elf
	vita-elf-create -e $(TARGET).yml $< $@

$(TARGET).elf: $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

.PHONY: clean send

clean:
	@rm -rf $(TARGET).skprx $(TARGET).velf $(TARGET).elf $(OBJS) $(DEPS)

send: $(TARGET).skprx
	curl -T $(TARGET).skprx ftp://$(PSVITAIP):1337/ux0:/data/tai/kplugin.skprx
	@echo "Sent."

launch: send
	echo "load_skprx ux0:/data/tai/kplugin.skprx" | nc $(PSVITAIP) 1338
	@echo "Launched!"

taisend: $(TARGET).skprx
	curl -T $(TARGET).skprx ftp://$(PSVITAIP):1337/ux0:/tai/$(TARGET).skprx
	@echo "Sent."

-include $(DEPS)
