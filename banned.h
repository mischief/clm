/* SPDX-License-Identifier: ISC */
#ifndef TMPL_BANNED_H
#define TMPL_BANNED_H

/*
 * Include this file last, after all system headers. #pragma GCC poison fires
 * on any token use including declarations inside system headers -- including
 * banned.h before them would poison the headers themselves.
 */

/* Dangerous string and input functions */
#pragma GCC poison gets strcpy strcat strncpy strncat strtok sprintf vsprintf

/* Unsafe integer parsers: no error detection */
#pragma GCC poison atoi atol atoll

/*
 * Buffered stdio file I/O: a write reports success from the buffer, so
 * ENOSPC/quota errors surface only at an often-unchecked close. Use raw
 * open/read/write/close. snprintf/vsnprintf/sscanf stay (buffers, no file).
 */
#pragma GCC poison FILE fopen fdopen freopen fclose fflush fileno fread fwrite fgets fgetc getc getchar ungetc fputs fputc putc putchar puts getline getdelim fprintf vfprintf printf vprintf fscanf scanf fseek ftell fsetpos fgetpos rewind setvbuf setbuf perror popen pclose tmpfile clearerr feof ferror

/* Stack smashing hazards */
#ifdef alloca
#undef alloca
#endif
#pragma GCC poison alloca

#endif /* TMPL_BANNED_H */
