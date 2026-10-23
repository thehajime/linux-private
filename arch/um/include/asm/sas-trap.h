/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_SAS_TRAP_H
#define __UM_SAS_TRAP_H

#include <linux/types.h>

struct uml_pt_regs;
struct siginfo;

/* Only the CONFIG_UML_NOMMU_SAS caller links these helpers. */
bool sas_user_ip(unsigned long ip);
#ifdef CONFIG_64BIT
bool sas_zpoline_invalid_syscall(struct uml_pt_regs *regs,
				 struct siginfo *si, void *mc);
#endif

#endif
