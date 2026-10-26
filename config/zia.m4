dnl # SPDX-License-Identifier: CDDL-1.0
dnl #
dnl # Z.I.A. (ZFS Interface for Accelerators) is built by default. It has
dnl # no build-time dependency on any accelerator framework: accelerators
dnl # are attached at runtime by a bridge module such as zia-dpusm.ko.
dnl #
dnl # --disable-zia / --without-zia   build ZFS without Z.I.A.
dnl # --with-zia=PATH                 also build the zia-dpusm bridge and
dnl #                                 the example zia-software-provider
dnl #                                 against the DPUSM build in PATH (same
dnl #                                 as "make zia-modules DPUSM=PATH")
dnl #
AC_DEFUN([ZFS_AC_ZIA], [
	AC_ARG_ENABLE([zia],
		AS_HELP_STRING([--disable-zia],
			[disable ZFS Interface for Accelerators support]),
		[], [enable_zia=yes])

	AC_ARG_WITH([zia],
		AS_HELP_STRING([--with-zia=PATH],
			[also build the Z.I.A. DPUSM bridge and example provider
			against the DPUSM build in PATH]),
		[
			AS_CASE(["$withval"],
				[no], [enable_zia=no; DPUSM_ROOT=""],
				[yes], [DPUSM_ROOT=""],
				[DPUSM_ROOT="$withval"])
		],
		[]
	)

	AS_IF([test "x$enable_zia" = "xyes"], [
		ZIA_CPPFLAGS="-DZIA=1"
		KERNEL_ZIA_CPPFLAGS="-DZIA=1"
		WITH_ZIA="_with_zia"
	], [
		AS_IF([test "x$DPUSM_ROOT" != "x"],
			[AC_MSG_ERROR([--with-zia=PATH cannot be used with --disable-zia])])
		WITH_ZIA="_without_zia"
	])

	AS_IF([test "x$DPUSM_ROOT" != "x"], [
		AS_IF([! test -d "$DPUSM_ROOT"],
			[AC_MSG_ERROR([--with-zia=PATH requires the DPUSM build directory])])
		DPUSM_ROOT=$(cd "$DPUSM_ROOT" && pwd)
		AS_IF([! test -r "$DPUSM_ROOT/Module.symvers"],
			[AC_MSG_ERROR([*** Failed to find Module.symvers in: $DPUSM_ROOT])])
		AS_IF([! test -r "$DPUSM_ROOT/include/dpusm/user_api.h"],
			[AC_MSG_ERROR([*** Failed to find include/dpusm/user_api.h in: $DPUSM_ROOT])])
	])

	AC_MSG_CHECKING([whether to build Z.I.A.])
	AC_MSG_RESULT([$enable_zia])

	AC_SUBST(WITH_ZIA)
	AC_SUBST(KERNEL_ZIA_CPPFLAGS)
	AC_SUBST(ZIA_CPPFLAGS)
	AC_SUBST(DPUSM_ROOT)
])
