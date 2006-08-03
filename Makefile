ifndef TP_MODULES
# This part runs as a normal, top-level Makefile:
X:=$(shell false)
KVER        := $(shell uname -r)
KSRC        := /lib/modules/$(KVER)/build
MOD_DIR     := /lib/modules/$(KVER)/kernel
PWD         := $(shell pwd)
IDIR        := include/linux
TP_DIR      := drivers/firmware
TP_MODULES  := tp_base.o tp_smapi.o
SHELL       := /bin/bash

ifeq ($(HDAPS),1)
TP_MODULES  += hdaps.o
LOAD_HDAPS  := insmod ./hdaps.ko
else
LOAD_HDAPS  := :
endif

DEBUG := 0

ifneq ($(shell [ -f $(KSRC)/include/linux/platform_device.h ] && echo 1),1)
$(error This driver requires kernel 2.6.15 or newer, and matching kernel headers.)
endif

.PHONY: default clean modules load unload install patch check_hdaps mk-hdaps.diff
export TP_MODULES

#####################################################################
# Main targets

default: modules

# Build the modules tp_base.ko, tp_smapi.ko and (if HDAPS=1) hdaps.ko
modules: $(KSRC) dmi_ec_oem_string.h $(patsubst %.o,%.c,$(TP_MODULES))
	$(MAKE) -C $(KSRC) M=$(PWD) modules

clean:
	rm -f tp_smapi.mod.* tp_smapi.o tp_smapi.ko .tp_smapi.*.cmd
	rm -f tp_base.mod.* tp_base.o tp_base.ko .tp_base.*.cmd
	rm -f hdaps.mod.* hdaps.o hdaps.ko .hdaps.*.cmd
	rm -f *~ diff/*~ *.orig diff/*.orig *.rej diff/*.rej
	rm -f tp_smapi-*-for-*.patch
	rm -fr .tmp_versions Modules.symvers diff/hdaps.diff.tmp
	rm -f dmi_ec_oem_string.h

load: check_hdaps unload modules
	@( [ `id -u` == 0 ] || { echo "Must be root to load modules"; exit 1; } )
	{ insmod ./tp_base.ko debug=$(DEBUG) &&\
	  insmod ./tp_smapi.ko debug=$(DEBUG) &&\
	  $(LOAD_HDAPS); }; :
	@echo -e '\nRecent dmesg output:' ; dmesg | tail -10

unload:
	@( [ `id -u` == 0 ] || { echo "Must be root to unload modules"; exit 1; } )
	if lsmod | grep -q '^hdaps '; then rmmod hdaps; fi
	if lsmod | grep -q '^tp_smapi '; then rmmod tp_smapi; fi
	if lsmod | grep -q '^tp_base '; then rmmod tp_base; fi

check_hdaps:
ifneq ($(HDAPS),1)
	@if lsmod | grep -q '^hdaps '; then \
	echo 'The hdaps driver is loaded. Use "make HDAPS=1 ..." to'\
	'patch hdaps for compatibility with tp_smapi.'\
	'This requires a kernel source tree.'; exit 1; fi
endif

install: modules
	@( [ `id -u` == 0 ] || { echo "Must be root to install modules"; exit 1; } )
	rm -f $(MOD_DIR)/$(TP_DIR)/{tp_base,tp_smapi}.ko
ifeq ($(HDAPS),1)
	rm -f $(MOD_DIR)/drivers/hwmon/hdaps.ko
endif
	$(MAKE) -C $(KSRC) M=$(PWD) modules_install
	depmod -a


# Ugly kludge for kernels that can't report OEM Strings DMI information:
ifeq ($(shell grep -q DMI_DEV_TYPE_OEM_STRING $(KSRC)/include/linux/dmi.h || echo 1),1)
ifeq ($(shell sudo /usr/sbin/dmidecode | grep -q dmidecode || echo 1),1)
$(error Could not run /usr/sbin/dmidecode. Must be root or get sudo password to do that.)
endif
GEN_DMI_DECODE_PATCH= \
  cat $(PWD)/diff/dmi-decode-and-save-oem-string-information.patch | \
  { [ -f $(KSRC)/drivers/firmware/dmi_scan.c ] && cat || \
    perl -pe 's@drivers/firmware/dmi_scan.c@arch/i386/kernel/dmi_scan.c@g'; }
DMI_EC_OEM_STRING:=$(shell sudo /usr/sbin/dmidecode | grep 'IBM ThinkPad Embedded Controller')
dmi_ec_oem_string.h: $(KSRC)/include/linux/dmi.h
	@echo 'WARNING: Your kernel does not have this patch applied:' >&2
	@echo '  dmi-decode-and-save-oem-string-information.patch' >&2
	@echo '  So I am hard-coding machine-specific DMI information into your driver.' >&2
	echo '/* This is machine-specific DMI information. */' > $@
	echo '#define DMI_EC_OEM_STRING_KLUDGE "$(DMI_EC_OEM_STRING)"' >> $@
else
NO_DMI_KLUDGE_DIFF=$(PWD)/diff/tp_base-no-dmi-kludge.diff
GEN_DMI_DECODE_PATCH=cat /dev/null
dmi_ec_oem_string.h: $(KSRC)/include/linux/dmi.h
	echo '/* Intentionally empty. You have proper DMI OEM Strings. */' > $@
endif

#####################################################################
# Generate a stand-alone kernel patch

TP_VER := ${shell sed -ne 's/^\#define TP_VERSION \"\(.*\)\"/\1/gp' tp_smapi.c}
ORG    := linux-$(KVER)-orig
NEW    := linux-$(KVER)-patched
PATCH  := tp_smapi-$(TP_VER)-for-$(KVER).patch

BASE_IN_PATCH  := 1
SMAPI_IN_PATCH := 1

patch:
	TMPDIR=`mktemp -d /tmp/tp_smapi-patch.XXXXXX` &&\
	echo "Work directory: $$TMPDIR" &&\
	cd $$TMPDIR &&\
	mkdir -p $(ORG)/$(TP_DIR) &&\
	mkdir -p $(ORG)/$(IDIR) &&\
	mkdir -p $(ORG)/drivers/hwmon &&\
	cp $(KSRC)/$(TP_DIR)/{Kconfig,Makefile} $(ORG)/$(TP_DIR) &&\
	cp $(KSRC)/drivers/hwmon/{Kconfig,hdaps.c} $(ORG)/drivers/hwmon/ &&\
	cp -r $(ORG) $(NEW) &&\
	\
	if [ "$(BASE_IN_PATCH)" == 1 ]; then \
	patch --no-backup-if-mismatch -s -d $(NEW) -i $(PWD)/diff/Kconfig-tp_base.diff -p1 &&\
	cp $(PWD)/tp_base.c $(NEW)/$(TP_DIR)/tp_base.c &&\
	cp $(PWD)/tp_base.h $(NEW)/$(IDIR)/tp_base.h &&\
	cp $(PWD)/hdaps.c $(NEW)/drivers/hwmon/ &&\
	sed -i -e '$$aobj-$$(CONFIG_TP_BASE)           += tp_base.o' $(NEW)/$(TP_DIR)/Makefile \
	; fi &&\
	\
	if [ "$(SMAPI_IN_PATCH)" == 1 ]; then \
	sed -i -e '$$aobj-$$(CONFIG_TP_SMAPI)          += tp_smapi.o' $(NEW)/$(TP_DIR)/Makefile &&\
	cp $(PWD)/tp_smapi.c $(NEW)/$(TP_DIR)/tp_smapi.c &&\
	patch --no-backup-if-mismatch -s -d $(NEW)/$(TP_DIR) -i $(PWD)/diff/tp_smapi-no_cd.diff -p1 &&\
	patch --no-backup-if-mismatch -s -d $(NEW)/$(TP_DIR) -i $(PWD)/diff/tp_base-no-dmi-kludge.diff -p1 &&\
	patch --no-backup-if-mismatch -s -d $(NEW) -i $(PWD)/diff/Kconfig-tp_smapi.diff -p1 &&\
	mkdir -p $(NEW)/Documentation &&\
	perl -0777 -pe 's/\n(Installation\n---+|Conflict with HDAPS\n---+|Files in this package\n---+|Setting and getting CD-ROM speed:\n).*?\n(?=[^\n]*\n-----)/\n/gs' $(PWD)/README > $(NEW)/Documentation/tp_smapi.txt \
	; fi &&\
	\
	{ diff -dNurp $(ORG) $(NEW) > patch \
	  || [ $$? -lt 2 ]; } &&\
	$(GEN_DMI_DECODE_PATCH) >> patch &&\
	{ diffstat patch; echo; echo; cat patch; } \
	  > $(PWD)/${PATCH} &&\
	rm -r $$TMPDIR
	@echo -e "\nPatch file created:\n  ${PATCH}"
	@echo -e "To apply, use:\n  patch -p1 -d ${KSRC} < ${PATCH}"

#####################################################################
# Tools for preparing a release. Ignore these.

set-version:
	perl -i -pe 's/^(tp_smapi version ).*/$${1}$(VER)/' README
	perl -i -pe 's/^(#define TP_VERSION ").*/$${1}$(VER)"/' tp_base.c tp_smapi.c

else
#####################################################################
# This part runs as a submake in kernel Makefile context:

CFLAGS := $(CFLAGS) -I$(M)/include
obj-m  := $(TP_MODULES)

endif
