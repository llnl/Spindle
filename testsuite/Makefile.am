noinst_PROGRAMS = libgenerator

ABS_TEST_DIR = $(abspath $(top_builddir)/testsuite)
BUILT_SOURCES = libtest10.so libtest11.so libtest12.so libtest13.so libtest14.so libtest15.so libtest16.so libtest17.so libtest18.so libtest19.so libtest20.so libtest50.so libtest100.so libtest500.so libtest1000.so libtest2000.so libtest4000.so libtest6000.so libtest8000.so libtest10000.so libtls1.c libtls2.c libtls3.c libtls4.c libtls5.c libtls6.c libtls7.c libtls8.c libtls9.c libtls10.c libtls11.c libtls12.c libtls13.c libtls14.c libtls15.c libtls16.c libtls17.c libtls18.c libtls19.c libtls20.c libsymlink.so libdepC.so libdepB.so libdepA.so libcxxexceptB.so libcxxexceptA.so origin_dir/liboriginlib.so origin_dir/origin_subdir/liborigintarget.so libtestoutput.so libfuncdict.so runTests run_driver run_driver_rm spindle.rc preload_file_list test_driver test_driver_libs retzero_rx retzero_r retzero_x retzero_ badinterp hello_r.py hello_x.py hello_rx.py hello_.py hello_l.py badlink.py spindle_exec_test spindle_deactivated.sh liblocal.so symbind_test interpreter_test interpreter_test_dir/interpreter_test_perl alias/aliastest.py exec_shell.sh exec_shell.tcsh exec_shell_expected_output exec_shell exec_shell_env.sh

BUILT_SOURCES += crash_test crash_test_fixedaddr crash_test_pie libcrashfuncs.so libcrashctor.so libcrashfixed.so run_crash_tests.sh

if BGQ_BLD
DYNAMIC_FLAG=-dynamic
IS_BLUEGENE=true
else
DYNAMIC_FLAG=
IS_BLUEGENE=false
endif

libgenerator_SOURCES = libgenerator.c

test_driverSOURCES = $(top_srcdir)/testsuite/test_driver.c
test_driverCFLAGS = -DLPATH=$(top_builddir)/testsuite -I$(top_srcdir)/src/client/spindle_api -I$(top_srcdir)/src/utils $(MPI_CFLAGS) -Wall -I$(top_builddir)
test_driverLDADD = -ldl -lpthread -ltestoutput -lfuncdict -lspindle
test_driverLDFLAGS = -Wl,-E -L$(top_builddir)/testsuite -L$(top_builddir)/src/client/spindle_api $(MPI_CLDFLAGS) $(LDFLAGS) -L. $(DYNAMIC_FLAG) -no-install

test_driver_libsSOURCES = $(top_srcdir)/testsuite/test_driver.c
test_driver_libsCFLAGS = -DLPATH=$(top_builddir)/testsuite -I$(top_srcdir)/src/client/spindle_api -I$(top_srcdir)/src/utils $(MPI_CFLAGS) -Wall
test_driver_libsLDADD = -ltest10 -ltest11 -ltest12 -ltest13 -ltest14 -ltest15 -ltest16 -ltest17 -ltest18 -ltest19 -ltest20 -ltest50 -ltest100 -ltest500 -ltest1000 -ltest2000 -ltest4000 -ltest6000 -ltest8000 -ltest10000 -ldepA -lcxxexceptA -loriginlib -ldl -lpthread -ltestoutput -lfuncdict -ldepB -ldepC -lcxxexceptB -lspindle
test_driver_libsLDFLAGS = -Wl,-E -L$(top_builddir)/testsuite $(MPI_CLDFLAGS) -L$(top_builddir)/src/client/spindle_api -L. $(DYNAMIC_FLAG) -no-install $(LDFLAGS) -Wl,-rpath,$(PWD)/origin_dir -L$(PWD)/origin_dir -Wl,-rpath-link,$(PWD)/origin_dir/origin_subdir -I$(top_builddir)

REGLIB_SRC = $(srcdir)/registerlib.c
LD_FUNCDICT = -L$(top_builddir)/testsuite -lfuncdict

test_driver: $(test_driverSOURCES) $(REGLIB_SRC) libtestoutput.so libfuncdict.so
	$(AM_V_CCLD) $(LIBTOOL) $(AM_V_lt) --tag=CC $(AM_LIBTOOLFLAGS) $(LIBTOOLFLAGS) --mode=link $(MPICC) -o $@ $(test_driverSOURCES) $(REGLIB_SRC) -DSO_NAME=$@ $(test_driverCFLAGS) $(test_driverLDFLAGS) $(test_driverLDADD)

test_driver_libs: $(test_driverSOURCES) $(REGLIB_SRC) libtestoutput.so libfuncdict.so libtest10.so libtest11.so libtest12.so libtest13.so libtest14.so libtest15.so libtest16.so libtest17.so libtest18.so libtest19.so libtest20.so libtest100.so libtest500.so libtest1000.so libtest2000.so libtest4000.so libtest6000.so libtest8000.so libtest10000.so libtls1.so libtls2.so libtls3.so libtls4.so libtls5.so libtls6.so libtls7.so libtls8.so libtls9.so libtls10.so libtls11.so libtls12.so libtls13.so libtls14.so libtls15.so libtls16.so libtls17.so libtls18.so libtls19.so libtls20.so libdepA.so libcxxexceptA.so libdepB.so libdepC.so libcxxexceptB.so origin_dir/liboriginlib.so
	$(AM_V_CCLD) $(LIBTOOL) $(AM_V_lt) --tag=CC $(AM_LIBTOOLFLAGS) $(LIBTOOLFLAGS) --mode=link $(MPICC) -o $@ $(test_driver_libsSOURCES) $(REGLIB_SRC) -DSO_NAME=$@ -DUSE_NEEDED $(test_driver_libsCFLAGS) $(test_driver_libsLDFLAGS) $(test_driver_libsLDADD)

libtest10.c: libgenerator
	$(AM_V_GEN)./libgenerator libtest10.c 10 t10

libtest11.c: libgenerator
	$(AM_V_GEN)./libgenerator libtest11.c 11 t11

libtest12.c: libgenerator
	$(AM_V_GEN)./libgenerator libtest12.c 12 t12

libtest13.c: libgenerator
	$(AM_V_GEN)./libgenerator libtest13.c 13 t13

libtest14.c: libgenerator
	$(AM_V_GEN)./libgenerator libtest14.c 14 t14

libtest15.c: libgenerator
	$(AM_V_GEN)./libgenerator libtest15.c 15 t15

libtest16.c: libgenerator
	$(AM_V_GEN)./libgenerator libtest16.c 16 t16

libtest17.c: libgenerator
	$(AM_V_GEN)./libgenerator libtest17.c 17 t17

libtest18.c: libgenerator
	$(AM_V_GEN)./libgenerator libtest18.c 18 t18

libtest19.c: libgenerator
	$(AM_V_GEN)./libgenerator libtest19.c 19 t19

libtest20.c: libgenerator
	$(AM_V_GEN)./libgenerator libtest20.c 20 t20

libtest50.c: libgenerator
	$(AM_V_GEN)./libgenerator libtest50.c 50 t50

libtest100.c: libgenerator
	$(AM_V_GEN)./libgenerator libtest100.c 100 t100

libtest500.c: libgenerator
	$(AM_V_GEN)./libgenerator libtest500.c 500 t500

libtest1000.c: libgenerator
	$(AM_V_GEN)./libgenerator libtest1000.c 1000 t1000

libtest2000.c: libgenerator
	$(AM_V_GEN)./libgenerator libtest2000.c 2000 t2000

libtest4000.c: libgenerator
	$(AM_V_GEN)./libgenerator libtest4000.c 4000 t4000

libtest6000.c: libgenerator
	$(AM_V_GEN)./libgenerator libtest6000.c 6000 t6000

libtest8000.c: libgenerator
	$(AM_V_GEN)./libgenerator libtest8000.c 8000 t8000

libtest10000.c: libgenerator
	$(AM_V_GEN)./libgenerator libtest10000.c 10000 t10000

libtls1.c: libgenerator
	$(AM_V_GEN)./libgenerator libtls1.c 1 u1

libtls2.c: libgenerator
	$(AM_V_GEN)./libgenerator libtls2.c 2 u2

libtls3.c: libgenerator
	$(AM_V_GEN)./libgenerator libtls3.c 3 u3

libtls4.c: libgenerator
	$(AM_V_GEN)./libgenerator libtls4.c 4 u4

libtls5.c: libgenerator
	$(AM_V_GEN)./libgenerator libtls5.c 5 u5

libtls6.c: libgenerator
	$(AM_V_GEN)./libgenerator libtls6.c 6 u6

libtls7.c: libgenerator
	$(AM_V_GEN)./libgenerator libtls7.c 7 u7

libtls8.c: libgenerator
	$(AM_V_GEN)./libgenerator libtls8.c 8 u8

libtls9.c: libgenerator
	$(AM_V_GEN)./libgenerator libtls9.c 9 u9

libtls10.c: libgenerator
	$(AM_V_GEN)./libgenerator libtls10.c 10 u10

libtls11.c: libgenerator
	$(AM_V_GEN)./libgenerator libtls11.c 11 u11

libtls12.c: libgenerator
	$(AM_V_GEN)./libgenerator libtls12.c 12 u12

libtls13.c: libgenerator
	$(AM_V_GEN)./libgenerator libtls13.c 13 u13

libtls14.c: libgenerator
	$(AM_V_GEN)./libgenerator libtls14.c 14 u14

libtls15.c: libgenerator
	$(AM_V_GEN)./libgenerator libtls15.c 15 u15

libtls16.c: libgenerator
	$(AM_V_GEN)./libgenerator libtls16.c 16 u16

libtls17.c: libgenerator
	$(AM_V_GEN)./libgenerator libtls17.c 17 u17

libtls18.c: libgenerator
	$(AM_V_GEN)./libgenerator libtls18.c 18 u18

libtls19.c: libgenerator
	$(AM_V_GEN)./libgenerator libtls19.c 19 u19

libtls20.c: libgenerator
	$(AM_V_GEN)./libgenerator libtls20.c 20 u20

libtest10.so: libtest10.c $(REGLIB_SRC) libfuncdict.so 
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DFUNC_NAME=t10_calc -DTLS_NAME=get_tlsvar_t10 $(LD_FUNCDICT)

libtest11.so: libtest11.c $(REGLIB_SRC) libfuncdict.so 
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DFUNC_NAME=t11_calc -DTLS_NAME=get_tlsvar_t11 $(LD_FUNCDICT)

libtest12.so: libtest12.c $(REGLIB_SRC) libfuncdict.so 
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DFUNC_NAME=t12_calc -DTLS_NAME=get_tlsvar_t12 $(LD_FUNCDICT)

libtest13.so: libtest13.c $(REGLIB_SRC) libfuncdict.so 
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DFUNC_NAME=t13_calc -DTLS_NAME=get_tlsvar_t13 $(LD_FUNCDICT)

libtest14.so: libtest14.c $(REGLIB_SRC) libfuncdict.so 
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DFUNC_NAME=t14_calc -DTLS_NAME=get_tlsvar_t14 $(LD_FUNCDICT)

libtest15.so: libtest15.c $(REGLIB_SRC) libfuncdict.so 
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DFUNC_NAME=t15_calc -DTLS_NAME=get_tlsvar_t15 $(LD_FUNCDICT)

libtest16.so: libtest16.c $(REGLIB_SRC) libfuncdict.so 
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DFUNC_NAME=t16_calc -DTLS_NAME=get_tlsvar_t16 $(LD_FUNCDICT)

libtest17.so: libtest17.c $(REGLIB_SRC) libfuncdict.so 
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DFUNC_NAME=t17_calc -DTLS_NAME=get_tlsvar_t17 $(LD_FUNCDICT)

libtest18.so: libtest18.c $(REGLIB_SRC) libfuncdict.so 
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DFUNC_NAME=t18_calc -DTLS_NAME=get_tlsvar_t18 $(LD_FUNCDICT)

libtest19.so: libtest19.c $(REGLIB_SRC) libfuncdict.so 
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DFUNC_NAME=t19_calc -DTLS_NAME=get_tlsvar_t19 $(LD_FUNCDICT)

libtest20.so: libtest20.c $(REGLIB_SRC) libfuncdict.so 
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DFUNC_NAME=t20_calc -DTLS_NAME=get_tlsvar_t20 $(LD_FUNCDICT)

libtest50.so: libtest50.c $(REGLIB_SRC) libfuncdict.so
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DFUNC_NAME=t50_calc -DTLS_NAME=get_tlsvar_t50 $(LD_FUNCDICT)

libtest100.so: libtest100.c $(REGLIB_SRC) libfuncdict.so
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DFUNC_NAME=t100_calc -DTLS_NAME=get_tlsvar_t100 $(LD_FUNCDICT)

libtest500.so: libtest500.c $(REGLIB_SRC) libfuncdict.so
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DFUNC_NAME=t500_calc -DTLS_NAME=get_tlsvar_t500 $(LD_FUNCDICT)

libtest1000.so: libtest1000.c $(REGLIB_SRC) libfuncdict.so
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DFUNC_NAME=t1000_calc -DTLS_NAME=get_tlsvar_t1000 $(LD_FUNCDICT)

libtest2000.so: libtest2000.c $(REGLIB_SRC) libfuncdict.so
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DFUNC_NAME=t2000_calc -DTLS_NAME=get_tlsvar_t2000 $(LD_FUNCDICT)

libtest4000.so: libtest4000.c $(REGLIB_SRC) libfuncdict.so
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DFUNC_NAME=t4000_calc -DTLS_NAME=get_tlsvar_t4000 $(LD_FUNCDICT)

libtest6000.so: libtest6000.c $(REGLIB_SRC) libfuncdict.so
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DFUNC_NAME=t6000_calc -DTLS_NAME=get_tlsvar_t6000 $(LD_FUNCDICT)

libtest8000.so: libtest8000.c $(REGLIB_SRC) libfuncdict.so
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DFUNC_NAME=t8000_calc -DTLS_NAME=get_tlsvar_t8000 $(LD_FUNCDICT)

libtest10000.so: libtest10000.c $(REGLIB_SRC) libfuncdict.so
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DFUNC_NAME=t10000_calc -DTLS_NAME=get_tlsvar_t10000 $(LD_FUNCDICT)

libtls1.so: libtls1.c $(REGLIB_SRC) libfuncdict.so 
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DTLS_NAME=get_tlsvar_u1 $(LD_FUNCDICT)

libtls2.so: libtls2.c $(REGLIB_SRC) libfuncdict.so 
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DTLS_NAME=get_tlsvar_u2 $(LD_FUNCDICT)

libtls3.so: libtls3.c $(REGLIB_SRC) libfuncdict.so 
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DTLS_NAME=get_tlsvar_u3 $(LD_FUNCDICT)

libtls4.so: libtls4.c $(REGLIB_SRC) libfuncdict.so 
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DTLS_NAME=get_tlsvar_u4 $(LD_FUNCDICT)

libtls5.so: libtls5.c $(REGLIB_SRC) libfuncdict.so 
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DTLS_NAME=get_tlsvar_u5 $(LD_FUNCDICT)

libtls6.so: libtls6.c $(REGLIB_SRC) libfuncdict.so 
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DTLS_NAME=get_tlsvar_u6 $(LD_FUNCDICT)

libtls7.so: libtls7.c $(REGLIB_SRC) libfuncdict.so 
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DTLS_NAME=get_tlsvar_u7 $(LD_FUNCDICT)

libtls8.so: libtls8.c $(REGLIB_SRC) libfuncdict.so 
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DTLS_NAME=get_tlsvar_u8 $(LD_FUNCDICT)

libtls9.so: libtls9.c $(REGLIB_SRC) libfuncdict.so 
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DTLS_NAME=get_tlsvar_u9 $(LD_FUNCDICT)

libtls10.so: libtls10.c $(REGLIB_SRC) libfuncdict.so 
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DTLS_NAME=get_tlsvar_u10 $(LD_FUNCDICT)

libtls11.so: libtls11.c $(REGLIB_SRC) libfuncdict.so 
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DTLS_NAME=get_tlsvar_u11 $(LD_FUNCDICT)

libtls12.so: libtls12.c $(REGLIB_SRC) libfuncdict.so 
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DTLS_NAME=get_tlsvar_u12 $(LD_FUNCDICT)

libtls13.so: libtls13.c $(REGLIB_SRC) libfuncdict.so 
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DTLS_NAME=get_tlsvar_u13 $(LD_FUNCDICT)

libtls14.so: libtls14.c $(REGLIB_SRC) libfuncdict.so 
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DTLS_NAME=get_tlsvar_u14 $(LD_FUNCDICT)

libtls15.so: libtls15.c $(REGLIB_SRC) libfuncdict.so 
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DTLS_NAME=get_tlsvar_u15 $(LD_FUNCDICT)

libtls16.so: libtls16.c $(REGLIB_SRC) libfuncdict.so 
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DTLS_NAME=get_tlsvar_u16 $(LD_FUNCDICT)

libtls17.so: libtls17.c $(REGLIB_SRC) libfuncdict.so 
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DTLS_NAME=get_tlsvar_u17 $(LD_FUNCDICT)

libtls18.so: libtls18.c $(REGLIB_SRC) libfuncdict.so 
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DTLS_NAME=get_tlsvar_u18 $(LD_FUNCDICT)

libtls19.so: libtls19.c $(REGLIB_SRC) libfuncdict.so 
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DTLS_NAME=get_tlsvar_u19 $(LD_FUNCDICT)

libtls20.so: libtls20.c $(REGLIB_SRC) libfuncdict.so 
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -DSO_NAME=$@ -DTLS_NAME=get_tlsvar_u20 $(LD_FUNCDICT)

libdepA.so: libdepA.c $(REGLIB_SRC) libdepB.so libfuncdict.so
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared -L. -ldepB -DSO_NAME=$@ -DFUNC_NAME=depA_calc  $(LD_FUNCDICT)

liblocal.so: local.c $(REGLIB_SRC) libfuncdict.so
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -shared $(LD_FUNCDICT) -DSO_NAME=$@ -DFUNC_NAME=check_hostname 

libsymlink.so: libtest10.so
	$(AM_V_GEN) ln -fs $< $@

libdepB.so: libdepB.c libdepC.so $(REGLIB_SRC)
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -DSO_NAME=$@ -shared -L. -ldepC

libdepC.so: libdepC.c $(REGLIB_SRC)
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -DSO_NAME=$@ -shared

registerlib_exceptA.o: $(REGLIB_SRC)
	$(AM_V_CC)$(MPICC) -c -o $@ -fPIC -Wall $< -DSO_NAME=libcxxexceptA.so -DFUNC_NAME=cxxexceptA_calc

registerlib_exceptB.o: $(REGLIB_SRC)
	$(AM_V_CC)$(MPICC) -c -o $@ -fPIC -Wall $< -DSO_NAME=libcxxexceptB.so

libcxxexceptA.so: libcxxexceptA.cc registerlib_exceptA.o libcxxexceptB.so libfuncdict.so
	$(AM_V_CCLD)$(MPICXX) -o $@ -fPIC -Wall $< registerlib_exceptA.o -shared -L. -lcxxexceptB -DSO_NAME=$@ -DFUNC_NAME=cxxexceptA_calc $(LD_FUNCDICT)

libcxxexceptB.so: libcxxexceptB.cc registerlib_exceptB.o 
	$(AM_V_CCLD)$(MPICXX) -o $@ -fPIC -Wall $< registerlib_exceptB.o -shared -DSO_NAME=$@

libtestoutput.so: testoutput.c $(REGLIB_SRC)
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -DSO_NAME=$@ -shared

libfuncdict.so: funcdict.c $(REGLIB_SRC)
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -DSO_NAME=$@ -shared

origin_dir/origin_subdir:
	$(AM_V_GEN) mkdir -p origin_dir/origin_subdir

origin_dir/origin_subdir/liborigintarget.so: origintarget.c origin_dir/origin_subdir
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< -shared
	@touch $@

origin_dir/liboriginlib.so: originlib.c $(REGLIB_SRC) origin_dir/origin_subdir/liborigintarget.so
	$(AM_V_CCLD)$(MPICC) -o $@ -fPIC -Wall $< $(REGLIB_SRC) -DSO_NAME=liboriginlib.so -DFUNC_NAME=origin_calc -shared '-Wl,-rpath,$$ORIGIN/origin_subdir' -Lorigin_dir/origin_subdir -lorigintarget

libsymbind_a.so: $(srcdir)/symbindlib.c $(srcdir)/symbind_fileops.c libsymbind_b.so
	$(AM_V_CCLD)$(CC) $(CFLAGS) -o $@ -shared -fPIC -Wall -DAUTORUN=a $(srcdir)/symbindlib.c -L. -Wl,--no-as-needed -lsymbind_b -I$(srcdir) -Wl,-rpath,$(top_builddir)/testsuite

libsymbind_b.so: $(srcdir)/symbindlib.c $(srcdir)/symbind_fileops.c
	$(AM_V_CCLD)$(CC) $(CFLAGS) -o $@ -shared -fPIC -Wall -DAUTORUN=b $(srcdir)/symbindlib.c -I$(srcdir) -Wl,-rpath,$(top_builddir)/testsuite

libsymbind_c.so: $(srcdir)/symbindlib.c $(srcdir)/symbind_fileops.c libsymbind_d.so
	$(AM_V_CCLD)$(CC) $(CFLAGS) -o $@ -shared -fPIC -Wall $(srcdir)/symbindlib.c -L. -lsymbind_d -I$(srcdir) -Wl,-rpath,$(top_builddir)/testsuite

libsymbind_d.so: $(srcdir)/symbindlib.c $(srcdir)/symbind_fileops.c
	$(AM_V_CCLD)$(CC) $(CFLAGS) -o $@ -shared -fPIC -Wall $(srcdir)/symbindlib.c -I$(srcdir) -Wl,-rpath,$(top_builddir)/testsuite

libsymbind_e.so: $(srcdir)/symbindlib.c $(srcdir)/symbind_fileops.c
	$(AM_V_CCLD)$(CC) $(CFLAGS) -o $@ -shared -fPIC -Wall $(srcdir)/symbindlib.c -I$(srcdir) -Wl,-rpath,$(top_builddir)/testsuite

libsymbind_f.so: $(srcdir)/symbindlib.c $(srcdir)/symbind_fileops.c
	$(AM_V_CCLD)$(CC) $(CFLAGS) -fno-plt -o $@ -shared -fPIC -Wall $(srcdir)/symbindlib.c -I$(srcdir) -Wl,-rpath,$(top_builddir)/testsuite

libsymbind_g.so: $(srcdir)/symbindlib_g.c
	$(AM_V_CC)$(CC) $(CFLAGS) -c -o symbindlib_g.o -shared -fPIC -fno-stack-protector -Wall $(srcdir)/symbindlib_g.c -I$(srcdir)
	$(AM_V_CCLD)$(LD) -shared -o $@ symbindlib_g.o -rpath $(top_builddir)/testsuite

symbind_test: $(srcdir)/symbind_test.c $(srcdir)/symbind_fileops.c libsymbind_a.so libsymbind_b.so libsymbind_c.so libsymbind_d.so libsymbind_e.so libsymbind_f.so libsymbind_g.so
	$(AM_V_CCLD)$(CC) $(CFLAGS) -o $@ -Wall $(srcdir)/symbind_test.c -L. -Wl,--no-as-needed -lsymbind_a -I$(srcdir) -ldl -Wl,-rpath,$(top_builddir)/testsuite

retzero_rx: retzero.c
	$(AM_V_CCLD)$(MPICC) -o $@ $<
	$(AM_V_GEN)chmod 700 $@

retzero_r: retzero.c
	$(AM_V_CCLD)$(MPICC) -o $@ $<
	$(AM_V_GEN)chmod 600 $@

retzero_x: retzero.c
	$(AM_V_CCLD)$(MPICC) -o $@ $<
	$(AM_V_GEN)chmod 300 $@

retzero_: retzero.c
	$(AM_V_CCLD)$(MPICC) -o $@ $<
	$(AM_V_GEN)chmod 200 $@

badinterp:
	$(AM_V_GEN)echo "#!/notexist" > $@; chmod 700 $@

hello_r.py: hello.py
	$(AM_V_GEN)cp $< $@; chmod 600 $@

hello_x.py: hello.py
	$(AM_V_GEN)cp $< $@; chmod 300 $@

hello_rx.py: hello.py
	$(AM_V_GEN)cp $< $@; chmod 700 $@

hello_.py: hello.py
	$(AM_V_GEN)cp $< $@; chmod 200 $@

hello_l.py:
	$(AM_V_GEN)ln -fs hello_x.py $@

badlink.py:
	@if [ ! -h $@ ] ; then \
		if [ $V ]; then echo ln -fs noexist.py $@; else echo "  GEN      $@"; fi; \
		ln -fs noexist.py $@; \
	fi

alias/aliastest.py: 
	$(AM_V_GEN)mkdir -p alias/real1/real2 ; echo "fail" > alias/aliastest.py ; echo "pass" > alias/real1/aliastest.py ; ln -s real1/real2 alias/lnk

spindle_deactivated.sh: spindle_deactivated_template.sh
	$(AM_V_GEN)cp $< $@; chmod 700 $@

spindle_exec_test: spindle_exec_test.c
	$(AM_V_CCLD)$(CC) $(CFLAGS) -o $@ $<

runTests: $(srcdir)/runTests_template
	@rm -f ./runTests
	$(AM_V_GEN)$(SED) -e s,BLUEGENE_TEST,$(IS_BLUEGENE),g\;s,TEST_RESOURCE_MANAGER,$(TESTRM),g\;s,SPINDLE_EXEC,$(bindir)/spindle,g < $(srcdir)/runTests_template > $(top_builddir)/testsuite/runTests
	@chmod 700 $(top_builddir)/testsuite/runTests

run_driver: $(srcdir)/run_driver_template $(top_builddir)/Makefile
	@rm -f ./run_driver
	$(AM_V_GEN)$(SED) -e s,SPINDLE_EXEC,$(bindir)/spindle,g\;s,TEST_RUN_DIR,$(ABS_TEST_DIR),g\;s,BLUEGENE_TEST,$(IS_BLUEGENE),g < $(srcdir)/run_driver_template > $(top_builddir)/testsuite/run_driver
	@chmod 700 $(top_builddir)/testsuite/run_driver

run_driver_rm: $(srcdir)/run_driver_$(TESTRM) $(top_builddir)/Makefile
	$(AM_V_GEN)cp $(srcdir)/run_driver_$(TESTRM) $(top_builddir)/testsuite/run_driver_rm
	@chmod 700 $(top_builddir)/testsuite/run_driver_rm

spindle.rc: $(srcdir)/spindle_rc $(top_builddir)/Makefile
	@rm -f ./spindle.rc
	$(AM_V_GEN)$(SED) -e s,SPINDLE_LIB,$(libdir),g < $(srcdir)/spindle_rc > $(top_builddir)/testsuite/spindle.rc

preload_file_list: $(srcdir)/preload_file_list_template $(top_builddir)/Makefile
	@rm -f ./preload_file_list
	$(AM_V_GEN)$(SED) -e s,TEST_RUN_DIR,$(ABS_TEST_DIR),g < $(srcdir)/preload_file_list_template > $(top_builddir)/testsuite/preload_file_list

interpreter_test_dir:
	$(AM_V_GEN) mkdir -p $@

interpreter_test_dir/interpreter_test_perl: interpreter_test_perl_template interpreter_test_dir
	$(AM_V_GEN)cp $< $@; chmod 700 $@

interpreter_test: interpreter_test_template
	$(AM_V_GEN)cp $< $@; chmod 700 $@

crash_test: $(srcdir)/crash_test.c $(srcdir)/crash_functions.c $(srcdir)/crash_functions.h libcrashfuncs.so libcrashctor.so libcrashfixed.so
	$(AM_V_CCLD)$(MPICC) $(CFLAGS) -O0 -fno-omit-frame-pointer -no-pie -Wl,-E \
		-o $@ $(srcdir)/crash_test.c $(srcdir)/crash_functions.c -I$(srcdir) -ldl -lpthread

CRASH_FIXED_EXE_ADDR ?= 0x60000000
crash_test_fixedaddr: $(srcdir)/crash_test.c $(srcdir)/crash_functions.c $(srcdir)/crash_functions.h
	$(AM_V_CCLD)$(MPICC) $(CFLAGS) -O0 -fno-omit-frame-pointer -no-pie -Wl,-E \
		-Wl,-Ttext-segment=$(CRASH_FIXED_EXE_ADDR) \
		-o $@ $(srcdir)/crash_test.c $(srcdir)/crash_functions.c -I$(srcdir) -ldl -lpthread

crash_test_pie: $(srcdir)/crash_test.c $(srcdir)/crash_functions.c $(srcdir)/crash_functions.h
	$(AM_V_CCLD)$(MPICC) $(CFLAGS) -O0 -fno-omit-frame-pointer -pie -fPIE -Wl,-E \
		-o $@ $(srcdir)/crash_test.c $(srcdir)/crash_functions.c -I$(srcdir) -ldl -lpthread

libcrashfuncs.so: $(srcdir)/libcrashfuncs.c
	$(AM_V_CCLD)$(CC) $(CFLAGS) -O0 -fno-omit-frame-pointer -shared -fPIC \
		-o $@ $(srcdir)/libcrashfuncs.c

libcrashctor.so: $(srcdir)/libcrashctor.c
	$(AM_V_CCLD)$(CC) $(CFLAGS) -O0 -fno-omit-frame-pointer -shared -fPIC \
		-o $@ $(srcdir)/libcrashctor.c

CRASH_FIXED_LIB_ADDR ?= 0x10000000
libcrashfixed.so: $(srcdir)/libcrashfixed.c
	$(AM_V_CCLD)$(CC) $(CFLAGS) -O0 -fno-omit-frame-pointer -shared -fPIC \
		-Wl,-Ttext-segment=$(CRASH_FIXED_LIB_ADDR) \
		-o $@ $(srcdir)/libcrashfixed.c

run_crash_tests.sh: $(srcdir)/run_crash_tests_template.sh $(top_builddir)/Makefile
	$(AM_V_GEN)$(SED) -e s,TEST_RESOURCE_MANAGER,$(TESTRM),g\;s,SPINDLE_EXEC,$(bindir)/spindle,g\;s,SPINDLE_RC_PATH,$(sysconfdir)/spindle/spindle.rc,g\;s,TEST_RUN_DIR,$(ABS_TEST_DIR),g < $< > $@; chmod 700 $@

EXTRA_DIST = run_crash_tests_template.sh crash_test.c crash_functions.c crash_functions.h libcrashfuncs.c libcrashctor.c libcrashfixed.c

exec_shell.sh: $(srcdir)/exec_shell_template.sh 
	$(AM_V_GEN)cp $< $@; chmod 700 $@

exec_shell.tcsh: $(srcdir)/exec_shell_template.tcsh 
	$(AM_V_GEN)cp $< $@; chmod 700 $@

exec_shell_env.sh: $(srcdir)/exec_shell_env_template.sh
	$(AM_V_GEN)cp $< $@; chmod 700 $@

exec_shell_expected_output: $(srcdir)/exec_shell_expected_output_template
	$(AM_V_GEN)cp $< $@

exec_shell: $(srcdir)/exec_shell.c
	$(AM_V_CCLD)$(CC) $(CFLAGS) -o $@ $<

CLEANFILES = libtest10.c libtest11.c libtest12.c libtest13.c libtest14.c libtest15.c libtest16.c libtest17.c libtest18.c libtest19.c libtest20.c libtest10.so libtest50.c libtest50.so libtest100.c libtest100.so libtest500.c libtest500.so libtest1000.c libtest1000.so libtest2000.c libtest2000.so libtest4000.c libtest4000.so libtest6000.c libtest6000.so libtest8000.c libtest8000.so libtest10000.c libtest10000.so libsymlink.so libdepA.so libdepB.so libdepC.so libcxxexceptA.so libcxxexceptB.so libtestoutput.so libfuncdict.so runTests run_driver run_driver_rm spindle.rc test_driver test_driver_libs preload_file_list retzero_rx retzero_r retzero_x retzero_ badinterp hello_r.py hello_x.py hello_rx.py hello_.py hello_l.py badlink.py libtls1.c libtls2.c libtls3.c libtls4.c libtls5.c libtls6.c libtls7.c libtls8.c libtls9.c libtls10.c libtls11.c libtls12.c libtls13.c libtls14.c libtls15.c libtls16.c libtls17.c libtls18.c libtls19.c libtls20.c libtls1.so libtls2.so libtls3.so libtls4.so libtls5.so libtls6.so libtls7.so libtls8.so libtls9.so libtls10.so libtls11.so libtls12.so libtls13.so libtls14.so libtls15.so libtls16.so libtls17.so libtls18.so libtls19.so libtls20.so symbind_test libsymbind_a.so libsymbind_b.so libsymbind_c.so libsymbind_d.so libsymbind_e.so libsymbind_f.so libsymbind_g.so interpreter_test interpreter_test_dir/interpreter_test_perl alias exec_shell.sh exec_shell.tcsh exec_shell_env.sh exec_shell_expected_output exec_shell crash_test crash_test_fixedaddr crash_test_pie libcrashfuncs.so libcrashctor.so libcrashfixed.so run_crash_tests.sh
