// SPDX-License-Identifier: ISC
/*
 * Force-included into every libclm source on the ESP-IDF port (-include).
 * newlib on esp-idf exports __getdelim/__getline but not the public getline/
 * getdelim, and its stdio.h does not declare them. We declare them here and
 * provide thin wrappers in compat_esp32.c.
 */
#ifndef CLM_COMPAT_H
#define CLM_COMPAT_H

#include <stdio.h>
#include <sys/types.h>

ssize_t getline(char **lineptr, size_t *n, FILE *stream);
ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *stream);

#endif
