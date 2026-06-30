// SPDX-License-Identifier: ISC
#ifndef CLM_EXPORT_H
#define CLM_EXPORT_H

#ifdef CLM_BUILDING
#define CLM_API __attribute__((visibility("default")))
#else
#define CLM_API
#endif

#endif
