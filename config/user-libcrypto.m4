dnl # SPDX-License-Identifier: CDDL-1.0
dnl #
dnl # Check for libcrypto. Used for userspace password derivation via PBKDF2.
dnl #
AC_DEFUN([ZFS_AC_CONFIG_USER_LIBCRYPTO], [
	ZFS_AC_FIND_SYSTEM_LIBRARY(LIBCRYPTO, [libcrypto], [openssl/evp.h], [], [crypto], [PKCS5_PBKDF2_HMAC_SHA1], [], [
		AC_MSG_FAILURE([
		*** evp.h missing, libssl-devel package required])])

	OPENSSL_ZALGO_ENABLED=no
	AS_IF([test "x$LIBCRYPTO_CFLAGS" != x || test "x$LIBCRYPTO_LIBS" != x], [
		saved_CPPFLAGS="$CPPFLAGS"
		saved_LIBS="$LIBS"
		CPPFLAGS="$CPPFLAGS $LIBCRYPTO_CFLAGS"
		LIBS="$LIBCRYPTO_LIBS $LIBS"
		AC_LINK_IFELSE([AC_LANG_PROGRAM([[
#include <openssl/evp.h>
#include <openssl/params.h>
]], [[
	EVP_MAC *mac = EVP_MAC_fetch(NULL, "HMAC", NULL);
	EVP_MAC_CTX *mctx = EVP_MAC_CTX_new(mac);
	EVP_CIPHER *cipher = EVP_CIPHER_fetch(NULL, "AES-128-GCM", NULL);
	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	unsigned char key[32] = { 0 };
	unsigned char iv[12] = { 0 };
	unsigned char input[16] = { 0 };
	unsigned char output[32] = { 0 };
	unsigned char tag[16] = { 0 };
	OSSL_PARAM params[2] = {
	    OSSL_PARAM_construct_utf8_string("digest", (char *)"SHA512", 0),
	    OSSL_PARAM_construct_end()
	};
	size_t maclen = 0;
	int len = 0;
	int result = (mac != NULL && mctx != NULL && cipher != NULL &&
	    ctx != NULL && EVP_CIPHER_CTX_copy(ctx, ctx) == 1 &&
	    EVP_MAC_init(mctx, key, sizeof (key), params) == 1 &&
	    EVP_MAC_update(mctx, input, sizeof (input)) == 1 &&
	    EVP_MAC_final(mctx, tag, &maclen, sizeof (tag)) == 1 &&
	    EVP_EncryptInit_ex(ctx, cipher, NULL, NULL, NULL) == 1 &&
	    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, sizeof (iv),
	    NULL) == 1 &&
	    EVP_EncryptInit_ex(ctx, NULL, NULL, key, iv) == 1 &&
	    EVP_EncryptUpdate(ctx, output, &len, input, sizeof (input)) == 1 &&
	    EVP_EncryptFinal_ex(ctx, output + len, &len) == 1 &&
	    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, sizeof (tag), tag) ==
	    1);
	EVP_CIPHER_CTX_free(ctx);
	EVP_CIPHER_free(cipher);
	EVP_MAC_CTX_free(mctx);
	EVP_MAC_free(mac);
	return (result ? 0 : 1);
]])], [OPENSSL_ZALGO_ENABLED=yes], [])
		CPPFLAGS="$saved_CPPFLAGS"
		LIBS="$saved_LIBS"
	])
	AM_CONDITIONAL([OPENSSL_ZALGO_ENABLED],
	    [test "x$OPENSSL_ZALGO_ENABLED" = "xyes"])
	AS_IF([test "x$OPENSSL_ZALGO_ENABLED" = "xyes"], [
		AC_DEFINE([OPENSSL_ZALGO_ENABLED], [1],
		    [Define when the OpenSSL Zalgo shim is available])
	])
])
