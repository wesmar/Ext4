/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_STDDEF_H
#define _LINUX_STDDEF_H

#ifndef offsetof
#define offsetof(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER)
#endif

#endif /* _LINUX_STDDEF_H */
