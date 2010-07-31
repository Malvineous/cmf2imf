bin_PROGRAMS = cmf2imf

cmf2imf_SOURCES = main.cpp cmf.cpp

AM_CPPFLAGS = $(BOOST_CPPFLAGS) $(libgamecommon_CFLAGS) -I $(top_srcdir)/include
AM_LDFLAGS = $(BOOST_SYSTEM_LIBS) $(BOOST_PROGRAM_OPTIONS_LIBS)
AM_LDFLAGS += $(libgamecommon_LIBS)
