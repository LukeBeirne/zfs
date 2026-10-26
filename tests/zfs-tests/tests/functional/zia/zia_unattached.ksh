#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0

#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# https://opensource.org/license/CDDL-1.0.
#

#
# Copyright (c) 2026, Triad National Security, LLC.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/zia/zia.kshlib

#
# DESCRIPTION:
#	Z.I.A. works without an attached accelerator
#
# STRATEGY:
#   1. Detach Z.I.A. by unloading the bridge and provider, if loaded
#   2. Check that zia_available is "no"
#   3. Check that no provider can be selected, including one whose
#      module exists but is not loaded (providers are never loaded on
#      demand)
#   4. Turn on all offloads, write data, and check that ZFS handled it
#   5. Reattach Z.I.A. if the modules are available
#

verify_runnable "global"

function cleanup
{
    rm -f "${FILENAME}"
    load_provider
}
log_onexit cleanup

log_must unload_provider
log_must test "$(zia_available)" = "no"

provider_was_loaded="no"
if module_loaded "${PROVIDER_MODULE}"
then
    provider_was_loaded="yes"
fi

log_mustnot zpool set zia_provider="${PROVIDER_MODULE}" "${TESTPOOL}"
log_mustnot zpool set zia_provider="${PROVIDER}" "${TESTPOOL}"
log_mustnot zpool set zia_provider="zia_no_such_provider" "${TESTPOOL}"
log_must test "$(zpool get -H -o value zia_provider "${TESTPOOL}")" = "-"

# setting zia_provider must not load anything
if [[ "${provider_was_loaded}" == "no" ]]
then
    log_mustnot module_loaded "${PROVIDER_MODULE}"
fi

log_must offload_all
log_must file_write -o create -f "${FILENAME}" -b "${BLOCKSZ}" -c 1024 \
    -d "${DATA}"
log_must verify_pool "${TESTPOOL}"
log_must check_pool_status "${TESTPOOL}" "errors" "No known data errors"

log_pass
