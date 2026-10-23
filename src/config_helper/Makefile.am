bin_PROGRAMS = spindle-config-helper

spindle_config_helper_SOURCES = spindle-config-helper.cc

spindle_config_helper_CPPFLAGS = -I$(top_srcdir)/src/include \
                                  -I$(top_srcdir)/src/fe/startup \
                                  -I$(top_srcdir)/src/fe/logging \
                                  -I$(top_srcdir)/src/utils \
                                  -I$(top_srcdir)/src/server/cache \
                                  -I$(top_srcdir)/src/server/comlib \
                                  -I$(top_srcdir)/src/cobo \
                                  -I$(top_srcdir)/src/fe/comlib \
                                  -DPKGSYSCONFDIR=\"$(PKGSYSCONF_DIR)\"

spindle_config_helper_LDADD = $(top_builddir)/src/fe/startup/libconfigparse.a \
                               $(top_builddir)/src/fe/logging/libspindleflogc.la \
                               -lpthread

man_MANS = man/spindle-config-helper.1
