/* SPDX-License-Identifier: ISC */
#ifndef TMPL_BANNED_H
#define TMPL_BANNED_H

/*
 * Include this file last, after all system headers. #pragma GCC poison fires
 * on any token use including declarations inside system headers -- including
 * banned.h before them would poison the headers themselves.
 */

/*
 * Dangerous string and input functions. Some libcs (Apple's, with
 * _FORTIFY_SOURCE) define these as macros; poisoning an existing macro
 * is an error, so drop the macro forms first.
 */
#undef gets
#undef strcpy
#undef strcat
#undef strncpy
#undef strncat
#undef strtok
#undef sprintf
#undef vsprintf
#pragma GCC poison gets strcpy strcat strncpy strncat strtok sprintf vsprintf

/* Unsafe integer parsers: no error detection */
#pragma GCC poison atoi atol atoll

/* Stack smashing hazards */
#ifdef alloca
#undef alloca
#endif
#pragma GCC poison alloca

#endif /* TMPL_BANNED_H */
