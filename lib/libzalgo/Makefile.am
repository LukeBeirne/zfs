noinst_LTLIBRARIES += libzalgo.la libzalgo_shim_fletcher.la libzalgo_shim_icp.la
CPPCHECKTARGETS    += libzalgo.la libzalgo_shim_fletcher.la libzalgo_shim_icp.la

if OPENSSL_ZALGO_ENABLED
noinst_LTLIBRARIES += libzalgo_shim_openssl.la
CPPCHECKTARGETS += libzalgo_shim_openssl.la
endif

if ZALGO_DPUSM_ENABLED
noinst_LTLIBRARIES += libzalgo_shim_dpusm.la
CPPCHECKTARGETS    += libzalgo_shim_dpusm.la
endif

libzalgo_la_CFLAGS  = $(AM_CFLAGS) $(KERNEL_CFLAGS) $(LIBRARY_CFLAGS)
libzalgo_la_CPPFLAGS  = $(AM_CPPFLAGS)

nodist_libzalgo_la_SOURCES = \
	module/zalgo/zalgo.c \
	module/zalgo/zalgo_bench.c

libzalgo_shim_fletcher_la_CFLAGS  = $(libzalgo_la_CFLAGS)
libzalgo_shim_fletcher_la_CPPFLAGS  = $(libzalgo_la_CPPFLAGS)

nodist_libzalgo_shim_fletcher_la_SOURCES = \
	module/zalgo/zalgo_shim_fletcher.c

libzalgo_shim_icp_la_CFLAGS  = $(libzalgo_la_CFLAGS)
libzalgo_shim_icp_la_CPPFLAGS  = $(libzalgo_la_CPPFLAGS)

nodist_libzalgo_shim_icp_la_SOURCES = \
	module/zalgo/zalgo_shim_icp.c

if OPENSSL_ZALGO_ENABLED
libzalgo_shim_openssl_la_CFLAGS  = $(libzalgo_la_CFLAGS) $(LIBCRYPTO_CFLAGS)
libzalgo_shim_openssl_la_CPPFLAGS  = $(libzalgo_la_CPPFLAGS)

nodist_libzalgo_shim_openssl_la_SOURCES = \
	module/zalgo/zalgo_shim_openssl.c
endif

libzalgo_shim_dpusm_la_CFLAGS = $(libzalgo_la_CFLAGS)
libzalgo_shim_dpusm_la_CPPFLAGS = $(libzalgo_la_CPPFLAGS) \
	$(ZALGO_DPUSM_CPPFLAGS)

nodist_libzalgo_shim_dpusm_la_SOURCES = \
	module/zalgo/zalgo_shim_dpusm.c
