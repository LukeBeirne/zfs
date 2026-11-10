dnl # SPDX-License-Identifier: CDDL-1.0
dnl #
dnl # Optional DPUSM support for Zalgo hardware providers.
dnl #
AC_DEFUN([ZFS_AC_CONFIG_ZALGO_DPUSM], [
	DPUSM_ROOT=
	DPUSM_SYMBOLS=
	ZALGO_DPUSM_CPPFLAGS=
	ZALGO_DPUSM_ENABLED=

	AC_ARG_WITH([dpusm],
		AS_HELP_STRING([--with-dpusm=PATH],
			[Path to the DPUSM root for Zalgo hardware providers]),
		[
			AS_IF([test "x$withval" = "xno"], [
				AC_MSG_NOTICE([Zalgo DPUSM support disabled])
			], [
				DPUSM_ROOT="$withval"
				AS_IF([! test -d "$DPUSM_ROOT"],
					[AC_MSG_ERROR([--with-dpusm=PATH requires the DPUSM root directory])])

				DPUSM_SYMBOLS="$DPUSM_ROOT/Module.symvers"
				AS_IF([! test -r "$DPUSM_SYMBOLS"],
					[AC_MSG_ERROR([
	*** Failed to find Module.symvers in:
	$DPUSM_SYMBOLS])])

				ZALGO_DPUSM_CPPFLAGS="-DZALGO_DPUSM=1 -I$DPUSM_ROOT/include"
				ZALGO_DPUSM_ENABLED=yes
				AC_DEFINE([HAVE_ZALGO_DPUSM], [1],
					[Define when Zalgo DPUSM providers are enabled.])
				AC_MSG_NOTICE([Zalgo DPUSM support enabled using $DPUSM_ROOT])
			])
		], [
			AC_MSG_NOTICE([Zalgo DPUSM support disabled])
		])

	AC_SUBST([DPUSM_ROOT])
	AC_SUBST([DPUSM_SYMBOLS])
	AC_SUBST([ZALGO_DPUSM_CPPFLAGS])
	AC_SUBST([ZALGO_DPUSM_ENABLED])
	AM_CONDITIONAL([ZALGO_DPUSM_ENABLED],
		[test "x$ZALGO_DPUSM_ENABLED" = "xyes"])
])
