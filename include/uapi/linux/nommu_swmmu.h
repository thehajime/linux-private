/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_NOMMU_SWMMU_H
#define _UAPI_LINUX_NOMMU_SWMMU_H

#define NOMMU_SWMMU_ACCESS_CHECKED	0U
#define NOMMU_SWMMU_ACCESS_SIGNAL	(1U << 0)
#define NOMMU_SWMMU_ACCESS_DYNAMIC	(1U << 1)
#define NOMMU_SWMMU_ACCESS_RETURN_VALUE	(1U << 2)

/*
 * nommu_swmmu_load(NULL, 2 * sizeof(__u64), bounds, QUERY_DIRECT_WINDOW).
 * Used alone, this query writes the immutable [start, end) alias window.
 * Every host page in it is either a current guest alias or an inaccessible
 * guard. Host mapping permissions enforce read/write/execute access. Other
 * backends, and UML without the optional reservation, return -EOPNOTSUPP.
 */
#define NOMMU_SWMMU_QUERY_DIRECT_WINDOW	(1U << 3)

#define NOMMU_SWMMU_ACCESS_MASK	\
	(NOMMU_SWMMU_ACCESS_SIGNAL | NOMMU_SWMMU_ACCESS_DYNAMIC | \
	 NOMMU_SWMMU_ACCESS_RETURN_VALUE)

#endif /* _UAPI_LINUX_NOMMU_SWMMU_H */
