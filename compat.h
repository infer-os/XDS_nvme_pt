/* SPDX-License-Identifier: GPL-2.0 */
#ifndef P2P_COMPAT_H_
#define P2P_COMPAT_H_

/*
 * Kernel flavor dispatch. The Makefile selects the flavor through the
 * KERNEL_FLAVOR variable (vanilla is the default) and sets the matching
 * P2P_COMPAT_* macro through ccflags-y. Exactly one flavor header is
 * included below.
 */
#if defined(P2P_COMPAT_VELINUX)
#include "compat_velinux.h"
#elif defined(P2P_COMPAT_UBUNTU)
#include "compat_ubuntu.h"
#else
#include "compat_vanilla.h"
#endif

#endif /* P2P_COMPAT_H_ */
