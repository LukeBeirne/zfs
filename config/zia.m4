dnl # SPDX-License-Identifier: CDDL-1.0
dnl # Adds optional Z.I.A. support to the configuration.
dnl #
dnl # Z.I.A. has no build-time dependency on an accelerator framework.
dnl # The optional zia-dpusm and zia-software-provider modules are built
dnl # separately with 'make zia-modules DPUSM=PATH'.
AC_DEFUN([ZFS_AC_ZIA], [
	AC_ARG_ENABLE([zia],
		AS_HELP_STRING([--disable-zia],
			[disable ZFS Interface for Accelerators support]),
		[], [enable_zia=yes])

	dnl # --with-zia and --without-zia are kept as aliases.
	AC_ARG_WITH([zia], [], [
		AS_CASE(["$withval"],
			[no], [enable_zia=no],
			[yes], [enable_zia=yes],
			[AC_MSG_ERROR(m4_normalize([--with-zia=PATH is no longer
			    supported; build the DPUSM modules with
			    'make zia-modules DPUSM=PATH']))])
	])

	AC_MSG_CHECKING([whether to enable Z.I.A.])
	AC_MSG_RESULT([$enable_zia])

	AS_IF([test "x$enable_zia" = "xyes"], [
		ZIA_CPPFLAGS="-DZIA=1"
		KERNEL_ZIA_CPPFLAGS="-DZIA=1"
		WITH_ZIA="_with_zia"
	])

	AC_SUBST(WITH_ZIA)
	AC_SUBST(KERNEL_ZIA_CPPFLAGS)
	AC_SUBST(ZIA_CPPFLAGS)
])
