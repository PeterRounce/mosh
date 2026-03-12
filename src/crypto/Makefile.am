AM_CPPFLAGS = -I$(top_srcdir)/ $(SODIUM_CFLAGS)
AM_CXXFLAGS = $(WARNING_CXXFLAGS) $(PICKY_CXXFLAGS) $(HARDEN_CFLAGS) $(MISC_CXXFLAGS) $(CODE_COVERAGE_CXXFLAGS)

noinst_LIBRARIES = libmoshcrypto.a

libmoshcrypto_a_SOURCES = \
	base64.cc \
	base64.h \
	byteorder.h \
	crypto.cc \
	crypto.h \
	prng.h
