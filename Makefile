ifndef TP_MODULES
# This part runs as a normal, top-level Makefile:
X:=$(shell false)
KVER        := $(shell uname -r)
KBASE       := /lib/modules/$(KVER)
KSRC        := $(KBASE)/source
KBUILD      := $(KBASE)/build
MOD_DIR     := $(KBASE)/kernel
PWD         := $(shell pwd)
IDIR        := include/linux
TP_DIR      := drivers/misc
TP_MODULES  := thinkpad_ec.o tp_smapi.o
SHELL       := /bin/bash

ifeq ($(HDAPS),1)
TP_MODULES  += hdaps.o
LOAD_HDAPS  := insmod ./hdaps.ko
else
LOAD_HDAPS  := :
endif

ifeq ($(FORCE_IO),1)
THINKPAD_EC_PARAM := force_io=1
else
THINKPAD_EC_PARAM := 
endif

DEBUG := 0

ifneq ($(shell [ -f $(KBUILD)/include/linux/aio_abi.h ] && echo 1),1)
$(warning Building tp_smapi requires Linux kernel 2.6.19 or newer, and matching kernel headers.)
$(warning You may need to override the following Make variables:)
$(warning .   KVER=$(KVER))
$(warning .   KBUILD=$(KBUILD))
$(warning .   MOD_DIR=$(MOD_DIR))
$(warning For "make patch", you may also need the full kernel sources, and may need to override:)
$(warning .   KSRC=$(KSRC))
$(error Missing kernel headers)
endif

.PHONY: default clean modules load unload install patch check_hdaps mk-hdaps.diff
export TP_MODULES

#####################################################################
# Main targets

default: modules

# Build the modules thinkpad_ec.ko, tp_smapi.ko and (if HDAPS=1) hdaps.ko
modules: $(KBUILD) $(patsubst %.o,%.c,$(TP_MODULES))
	$(MAKE) -C $(KBUILD) M=$(PWD) O=$(KBUILD) modules

clean:
	rm -f tp_smapi.mod.* tp_smapi.o tp_smapi.ko .tp_smapi.*.cmd
	rm -f thinkpad_ec.mod.* thinkpad_ec.o thinkpad_ec.ko .thinkpad_ec.*.cmd
	rm -f hdaps.mod.* hdaps.o hdaps.ko .hdaps.*.cmd
	rm -f *~ diff/*~ *.orig diff/*.orig *.rej diff/*.rej
	rm -f tp_smapi-*-for-*.patch
	rm -fr .tmp_versions Modules.symvers diff/hdaps.diff.tmp

load: check_hdaps unload modules
	@( [ `id -u` == 0 ] || { echo "Must be root to load modules"; exit 1; } )
	{ insmod ./thinkpad_ec.ko $(THINKPAD_EC_PARAM) && insmod ./tp_smapi.ko debug=$(DEBUG) && $(LOAD_HDAPS); }; :
	@echo -e '\nRecent dmesg output:' ; dmesg | tail -10

unload:
	@( [ `id -u` == 0 ] || { echo "Must be root to unload modules"; exit 1; } )
	if lsmod | grep -q '^hdaps '; then rmmod hdaps; fi
	if lsmod | grep -q '^tp_smapi '; then rmmod tp_smapi; fi
	if lsmod | grep -q '^thinkpad_ec '; then rmmod thinkpad_ec; fi
	if lsmod | grep -q '^tp_base '; then rmmod tp_base; fi  # old thinkpad_ec

check_hdaps:
ifneq ($(HDAPS),1)
	@if lsmod | grep -q '^hdaps '; then \
	echo 'The hdaps driver is loaded. Use "make HDAPS=1 ..." to'\
	'patch hdaps for compatibility with tp_smapi.'\
	'This requires a kernel source tree.'; exit 1; fi
endif

install: modules
	@( [ `id -u` == 0 ] || { echo "Must be root to install modules"; exit 1; } )
	rm -f $(MOD_DIR)/$(TP_DIR)/{thinkpad_ec,tp_smapi,tp_base}.ko
	rm -f $(MOD_DIR)/drivers/firmware/{thinkpad_ec,tp_smapi,tp_base}.ko
	rm -f $(MOD_DIR)/extra/{thinkpad_ec,tp_smapi,tp_base}.ko
ifeq ($(HDAPS),1)
	rm -f $(MOD_DIR)/drivers/hwmon/hdaps.ko
	rm -f $(MOD_DIR)/extra/hdaps.ko
endif
	$(MAKE) -C $(KBUILD) M=$(PWD) O=$(KBUILD) modules_install
	depmod -a


#####################################################################
# Generate a stand-alone kernel patch

TP_VER := ${shell sed -ne 's/^\#define TP_VERSION \"\(.*\)\"/\1/gp' tp_smapi.c}
ORG    := a
NEW    := b
PATCH  := tp_smapi-$(TP_VER)-for-$(KVER).patch

BASE_IN_PATCH  := 1
SMAPI_IN_PATCH := 1
HDAPS_IN_PATCH := 1

patch: $(KSRC)
	@TMPDIR=`mktemp -d /tmp/tp_smapi-patch.XXXXXX` &&\
	echo "Working directory: $$TMPDIR" &&\
	cd $$TMPDIR &&\
	mkdir -p $(ORG)/$(TP_DIR) &&\
	mkdir -p $(ORG)/$(IDIR) &&\
	mkdir -p $(ORG)/drivers/hwmon &&\
	cp $(KSRC)/$(TP_DIR)/{Kconfig,Makefile} $(ORG)/$(TP_DIR) &&\
	cp $(KSRC)/drivers/hwmon/{Kconfig,hdaps.c} $(ORG)/drivers/hwmon/ &&\
	cp -r $(ORG) $(NEW) &&\
	\
	if [ "$(BASE_IN_PATCH)" == 1 ]; then \
		cp $(PWD)/thinkpad_ec.c $(NEW)/$(TP_DIR)/thinkpad_ec.c &&\
		cp $(PWD)/thinkpad_ec.h $(NEW)/$(IDIR)/thinkpad_ec.h &&\
		perl -i -pe 'print `cat $(PWD)/diff/Kconfig-thinkpad_ec.add` if m/^(endmenu|endif # MISC_DEVICES)$$/' $(NEW)/$(TP_DIR)/Kconfig &&\
		sed -i -e '$$aobj-$$(CONFIG_THINKPAD_EC)       += thinkpad_ec.o' $(NEW)/$(TP_DIR)/Makefile \
	; fi &&\
	\
	if [ "$(HDAPS_IN_PATCH)" == 1 ]; then \
		cp $(PWD)/hdaps.c $(NEW)/drivers/hwmon/ &&\
		perl -i -0777 -pe 's/(config SENSORS_HDAPS\n\ttristate [^\n]+\n\tdepends [^\n]+\n)/$$1\tselect THINKPAD_EC\n/' $(NEW)/drivers/hwmon/Kconfig  \
	; fi &&\
	\
	if [ "$(SMAPI_IN_PATCH)" == 1 ]; then \
		sed -i -e '$$aobj-$$(CONFIG_TP_SMAPI)          += tp_smapi.o' $(NEW)/$(TP_DIR)/Makefile &&\
		perl -i -pe 'print `cat $(PWD)/diff/Kconfig-tp_smapi.add` if m/^(endmenu|endif # MISC_DEVICES)$$/' $(NEW)/$(TP_DIR)/Kconfig &&\
		cp $(PWD)/tp_smapi.c $(NEW)/$(TP_DIR)/tp_smapi.c &&\
		mkdir -p $(NEW)/Documentation &&\
		perl -0777 -pe 's/\n(Installation\n---+|Conflict with HDAPS\n---+|Files in this package\n---+|Setting and getting CD-ROM speed:\n).*?\n(?=[^\n]*\n-----)/\n/gs' $(PWD)/README > $(NEW)/Documentation/tp_smapi.txt \
	; fi &&\
	\
	{ diff -dNurp $(ORG) $(NEW) > patch \
	  || [ $$? -lt 2 ]; } &&\
	{ echo "Generated for $(KVER) in $(KSRC)"; echo; diffstat patch; echo; echo; cat patch; } \
	  > $(PWD)/${PATCH} &&\
	rm -r $$TMPDIR
	@echo
	@diffstat ${PATCH}
	@echo -e "\nPatch file created:\n  ${PATCH}"
	@echo -e "To apply, use:\n  patch -p1 -d ${KSRC} < ${PATCH}"

#####################################################################
# Tools for preparing a release. Ignore these.

set-version:
	perl -i -pe 's/^(tp_smapi version ).*/$${1}$(VER)/' README
	perl -i -pe 's/^(#define TP_VERSION ").*/$${1}$(VER)"/' thinkpad_ec.c tp_smapi.c

TGZ=../tp_smapi-$(VER).tgz
create-tgz:
	git-archive  --format=tar --prefix=tp_smapi-$(VER)/ HEAD | gzip -c > $(TGZ)
	tar tzvf $(TGZ)
	echo "Ready: $(TGZ)"

else
#####################################################################
# This part runs as a submake in kernel Makefile context:

EXTRA_CFLAGS := $(CFLAGS) -I$(M)/include
obj-m        := $(TP_MODULES)

endif
