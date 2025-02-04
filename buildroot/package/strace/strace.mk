#############################################################
#
# strace
#
#############################################################
STRACE_VER:=4.12
STRACE_SOURCE:=strace-$(STRACE_VER).tar.xz
STRACE_SITE:=https://sourceforge.net/projects/strace/files/strace/$(STRACE_VER)/strace-$(STRACE_VER).tar.xz
#we must hardcoded 'xzcat' here because it's not defined (buildroot/config)
STRACE_CAT:="xzcat"
STRACE_DIR:=$(BUILD_DIR)/strace-$(STRACE_VER)

BR2_STRACE_CFLAGS:=
ifeq ($(BR2_LARGEFILE),)
BR2_STRACE_CFLAGS+=-U_LARGEFILE64_SOURCE -U__USE_LARGEFILE64 -U__USE_FILE_OFFSET64
endif

$(DL_DIR)/$(STRACE_SOURCE):
	 $(WGET) -P $(DL_DIR) $(STRACE_SITE)

strace-source: $(DL_DIR)/$(STRACE_SOURCE)

$(STRACE_DIR)/.unpacked: $(DL_DIR)/$(STRACE_SOURCE)
	$(STRACE_CAT) $(DL_DIR)/$(STRACE_SOURCE) | tar -C $(BUILD_DIR) $(TAR_OPTIONS) -
	toolchain/patch-kernel.sh $(STRACE_DIR) package/strace strace\*.patch
	touch $(STRACE_DIR)/.unpacked

$(STRACE_DIR)/.configured: $(STRACE_DIR)/.unpacked
	(cd $(STRACE_DIR); rm -rf config.cache; \
		$(if $(BR2_LARGEFILE),ac_cv_type_stat64=yes,ac_cv_type_stat64=no) \
		$(TARGET_CONFIGURE_OPTS) \
		CFLAGS="$(TARGET_CFLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS)" \
		aaa_ac_cv_header_linux_if_packet_h=yes \
		./configure \
		--target=$(GNU_TARGET_NAME) \
		--host=$(GNU_TARGET_NAME) \
		--build=$(GNU_HOST_NAME) \
		--prefix=/usr \
		--exec-prefix=/usr \
		--bindir=/usr/bin \
		--sbindir=/usr/sbin \
		--libdir=/lib \
		--libexecdir=/usr/lib \
		--sysconfdir=/etc \
		--datadir=/usr/share \
		--localstatedir=/var \
		--mandir=/usr/man \
		--infodir=/usr/info \
		$(DISABLE_LARGEFILE) \
	);
	touch $(STRACE_DIR)/.configured

$(STRACE_DIR)/strace: $(STRACE_DIR)/.configured
	$(MAKE) CC=$(TARGET_CC) -C $(STRACE_DIR)

$(TARGET_DIR)/usr/bin/strace: $(STRACE_DIR)/strace
	install -c $(STRACE_DIR)/strace $(TARGET_DIR)/usr/bin/strace
	$(STRIP) $(TARGET_DIR)/usr/bin/strace > /dev/null 2>&1
ifeq ($(strip $(BR2_CROSS_TOOLCHAIN_TARGET_UTILS)),y)
	mkdir -p $(STAGING_DIR)/$(GNU_TARGET_NAME)/target_utils
	install -c $(TARGET_DIR)/usr/bin/strace \
		$(STAGING_DIR)/$(GNU_TARGET_NAME)/target_utils/strace
endif

strace: uclibc $(TARGET_DIR)/usr/bin/strace 

strace-clean: 
	rm -f $(TARGET_DIR)/usr/bin/strace
	$(MAKE) -C $(STRACE_DIR) clean

strace-dirclean: 
	rm -rf $(STRACE_DIR) 


#############################################################
#
# Toplevel Makefile options
#
#############################################################
ifeq ($(strip $(BR2_PACKAGE_STRACE)),y)
TARGETS+=strace
endif
