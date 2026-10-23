bin_PROGRAMS = spindle
lib_LTLIBRARIES = libspindlefe.la
noinst_LTLIBRARIES = libspindlefe_static.la
noinst_LIBRARIES = libconfigparse.a

include_HEADERS = $(top_srcdir)/../include/spindle_launch.h

AM_CPPFLAGS = -I$(top_srcdir)/../logging

CORE_SOURCES = spindle_fe.cc parseargs.cc config_parser.cc config_mgr.cc parse_preload.cc $(top_srcdir)/../utils/pathfn.c $(top_srcdir)/../utils/keyfile.c $(top_srcdir)/../utils/parseloc.c $(top_srcdir)/../utils/rshlaunch.c
CORE_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_srcdir)/../include -I$(top_srcdir)/comlib -I$(top_srcdir)/../server/cache -I$(top_srcdir)/../server/comlib -I$(top_srcdir)/../utils -I$(top_srcdir)/../cobo -DBINDIR=\"$(pkglibexecdir)\" -DLIBEXECDIR=\"$(pkglibexecdir)\" -DPROGLIBDIR=\"$(pkglibdir)\" -DPKGSYSCONFDIR=\"$(PKGSYSCONF_DIR)\"
CORE_LDADD = $(top_builddir)/logging/libspindleflogc.la -lpthread
if COBO
CORE_LDADD += $(top_builddir)/comlib/libfe_cobo.la $(top_builddir)/cobo/libldcs_cobo.la
endif
if MSOCKET
CORE_LDADD += $(top_builddir)/comlib/libfe_msocket.la
endif
CORE_LDADD += $(MUNGE_DYN_LIB) $(GCRYPT_LIBS)

libspindlefe_la_CPPFLAGS = $(CORE_CPPFLAGS) -DSPINDLEFELIB
libspindlefe_la_SOURCES = $(CORE_SOURCES)
libspindlefe_la_LIBADD = $(CORE_LDADD)
libspindlefe_la_LDFLAGS = -version-info $(SPINDLEFE_LIB_VERSION)

libspindlefe_static_la_CPPFLAGS = $(libspindlefe_la_CPPFLAGS)
libspindlefe_static_la_SOURCES = $(libspindlefe_la_SOURCES)
libspindlefe_static_la_LIBADD = $(libspindlefe_la_LIBADD)


spindle_CPPFLAGS = $(CORE_CPPFLAGS) -DSPINDLEEXE
spindle_SOURCES = spindle_fe_main.cc spindle_fe_serial.cc parse_launcher.cc parse_launcher_args.cc launcher.cc spindle_session.cc launch_slurm.cc launch_lsf.cc $(CORE_SOURCES)

spindle_LDADD = $(CORE_LDADD) $(top_builddir)/openmpi_intercept/libparseompi.la $(top_builddir)/hostbin/libhostbin.la

if BLD_FLUXPLUGIN
spindle_LDADD += $(FLUX_CORE_LIBS) $(FLUX_HOSTLIST_LIBS) $(top_builddir)/flux/libfluxsessionfe.la
spindle_LDFLAGS = $(fluxplugin-ldflags) -Wl,--allow-multiple-definition
spindle_CPPFLAGS += -I$(top_srcdir)/../flux
spindle_SOURCES += launch_flux.cc
endif

if LMON
spindle_LDADD += $(top_builddir)/launchmon/libfelmon.la
endif

# Private static library for config parsing (used by spindle-config-helper)
libconfigparse_a_SOURCES = config_mgr.cc config_parser.cc parseargs.cc \
                           $(top_srcdir)/../utils/pathfn.c \
                           $(top_srcdir)/../utils/keyfile.c \
                           $(top_srcdir)/../utils/parseloc.c
libconfigparse_a_CPPFLAGS = $(CORE_CPPFLAGS)

