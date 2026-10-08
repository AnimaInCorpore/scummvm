/* Copyright (C) 2026 Sascha Springer
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "../../platform/atari/symbols.h"

	.text
	.even
	.globl SYM(falcon_opl_timer_a)
SYM(falcon_opl_timer_a):
| Delivery is integer-only. Leave the interrupted FPU state untouched.
	movem.l d0-d7/a0-a6,-(sp)
	jsr SYM(falcon_opl_delivery_tick)
	movem.l (sp)+,d0-d7/a0-a6
	rte
