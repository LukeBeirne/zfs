dnl # SPDX-License-Identifier: CDDL-1.0
dnl # Adds optional Z.I.A. support to the configuration.
dnl # The DPUSM path is only needed for the optional in-tree provider.
AC_DEFUN([ZFS_AC_ZIA], [
	AC_ARG_ENABLE([zia],
		AS_HELP_STRING([--disable-zia],
			[disable ZFS Interface for Accelerators support]),
		[], [enable_zia=yes])

	AC_ARG_WITH([zia],
		AS_HELP_STRING([--with-zia=PATH],
			[Path to DPUSM (for the optional software provider)]),
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
	])

	AS_IF([test "x$DPUSM_ROOT" != "x"], [
		AS_IF([! test -d "$DPUSM_ROOT"],
			[AC_MSG_ERROR([--with-zia=PATH requires the DPUSM root directory])])
		DPUSM_SYMBOLS="$DPUSM_ROOT/Module.symvers"
		AS_IF([! test -r "$DPUSM_SYMBOLS"],
			[AC_MSG_ERROR([*** Failed to find Module.symvers in: $DPUSM_SYMBOLS])])
	])

	AC_SUBST(WITH_ZIA)
	AC_SUBST(KERNEL_ZIA_CPPFLAGS)
	AC_SUBST(ZIA_CPPFLAGS)
	AC_SUBST(DPUSM_SYMBOLS)
	AC_SUBST(DPUSM_ROOT)
])
