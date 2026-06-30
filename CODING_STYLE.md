# Coding Style

This document describes the coding conventions for this project. Formatting
follows BSD Kernel Normal Form (KNF) as documented in style(9). Higher-level
conventions draw from the systemd Coding Style. Safety-critical discipline is
drawn from the JPL Institutional Coding Standard and the Power of Ten rules
(Holzmann, IEEE Computer, June 2006).

---

## Language Standard

- All code shall conform to **C17** (ISO/IEC 9899:2018).
- Target **IEEE Std 1003.1-2024** (POSIX.1-2024) as the platform interface.
  Non-POSIX extensions are allowed only where necessary and must be isolated
  and commented.
- Avoid external library dependencies. Prefer the C standard library and
  POSIX interfaces. When a dependency is genuinely warranted, choose an
  established, widely-available library.
- No undefined behavior. No reliance on implementation-defined behavior
  without explicit justification in a comment.

---

## Build and Tooling

- All code shall compile cleanly with `-Wall -Wextra -Werror`. Zero warnings,
  zero exceptions. If the compiler is wrong, rewrite the code until it is
  clearly valid.
- Run under UBSan and ASan regularly:
  ```sh
  meson setup build-asan -Db_sanitize=address,undefined
  meson test -C build-asan
  ```
- Fuzz harnesses are required for all code that parses external input.
- No VLAs (`-Wvla`). No `alloca`. These eliminate a class of stack overflow
  bugs and make stack usage statically analyzable.

---

## Formatting

This project uses **BSD Kernel Normal Form (KNF)** indentation.

### Indentation and whitespace

- **Tabs** for indentation, not spaces. One tab per level.
- Lines should not exceed **100 characters**. Break before that, not after.
- No trailing whitespace. Files end with a single newline.
- LF (`\n`) line endings only.

### Braces

Function definitions: opening brace on its own line.

```c
int
foo(const char *s)
{
        ...
}
```

Control structures: space between the keyword and the opening paren; opening
brace on the same line.

```c
if (condition) {
        foo();
        bar();
} else {
        baz();
}
```

Single-statement bodies may omit braces, but always add them when the body
spans multiple lines or when mixing with an `else`:

```c
if (p == NULL)
        return -EINVAL;

if (condition) {
        foo();
} else {
        bar();
        baz();
}
```

`switch`: `case` labels at the same indentation as `switch`; body indented
once. Every `switch` must have a `default`. Intentional fall-through must be
marked with a comment.

```c
switch (ch) {
case 'h':
        usage();
        break;
case 'v':
        /* fall through */
case 'V':
        print_version();
        break;
default:
        usage();
}
```

### Pointers, casts, and declarations

In declarations, `*` binds to the **name**, not the type. Declare one variable
per line.

```c
char *p;
char *q;    /* not: char *p, *q; */
```

KNF traditionally tab-aligns the `*` and name across a declaration block.
`clang-format` cannot reproduce this correctly (it uses spaces, not tabs, for
mid-line alignment), so it is not enforced here. Write it by hand if you like
it; do not use `AlignConsecutiveDeclarations`.

In casts, `*` binds to the type:

```c
p = (char *)buf;
```

Function return type goes on its own line for definitions:

```c
static int
parse_input(const char *buf, size_t len)
{
        ...
}
```

In prototypes (headers), the return type stays on the same line:

```c
int parse_input(const char *buf, size_t len);
```

### Comments

Use `/* */` for committed explanatory comments. Reserve `//` for temporary
debug annotations that should not be committed.

Err on the side of fewer comments. If the code needs a comment to be
understood, first consider rewriting it so it is clearer. Comments that
restate what the code does are noise; comments that explain *why* something
is done the way it is are valuable.

The most useful comments introduce a section of code, explain the purpose of
a global variable or data structure, or document a non-obvious constraint or
external reference:

```c
/* Retry on EINTR; SA_RESTART is not set for this handler. */
```

Avoid decorative banners and block comments. Avoid commenting every line.
When an algorithm comes from a paper or external reference, cite it.

Mark things that need attention with `/* XXX */` (risky, broken, or
surprising) or `/* TODO */` (incomplete but not immediately dangerous).
Both grep well and have a long history in Unix codebases.

Source files must contain only ASCII. Non-ASCII characters are not permitted
in comments, identifiers, or anywhere else in source code. The only
legitimate use of non-ASCII is UTF-8 encoded text in character or string
constants.

---

## File Layout

### Header files

Every header file must begin with an SPDX license identifier and use an
include guard (not `#pragma once`):

```c
// SPDX-License-Identifier: ISC
#ifndef MYLIB_FOO_H
#define MYLIB_FOO_H

...

#endif /* MYLIB_FOO_H */
```

Public headers expose only what callers need: opaque struct declarations,
public function prototypes, and essential macros. Keep headers as lean as
possible to minimize recompilation. No private headers shall be included from
a public header.

No `extern` variable declarations in headers unless unavoidable.

No circular header dependencies.

### Source files

Every source file begins with an SPDX license identifier.

Include order, with a blank line between groups, alphabetical within each:

1. System/standard headers (`<stdio.h>`, `<stdlib.h>`, ...)
2. POSIX headers (`<unistd.h>`, `<fcntl.h>`, ...)
3. Project public headers (`"mylib/foo.h"`)
4. Project internal headers (`"lib/internal.h"`, `"banned.h"`)

`banned.h` must come after all system headers. `#pragma GCC poison` fires on
any token use, including declarations inside system headers -- including it
before them would poison the headers themselves. Internal headers that include
`banned.h` (such as `lib/internal.h`) satisfy this automatically as long as
they are included after system headers, which this ordering guarantees.

No source file shall `#include` another source file.

Section order within a source file:

1. Includes
2. Internal type and macro definitions
3. `static` variable declarations
4. `static` function prototypes
5. Public function implementations
6. `static` function implementations

---

## Naming

- All function and variable names: `lowercase_with_underscores`.
- Name length should match scope. Loop indices and short-lived locals can be
  single letters (`i`, `n`, `p`, `c`). Globals and exported names need enough
  context to be unambiguous at the point of use. Prefer minimum length with
  maximum information; let context fill in the rest.
- Public API symbols: prefixed with the library name (`tmpl_new`,
  `tmpl_ref`, `tmpl_unref`).
- File-local symbols must be `static`.
- Type names end in `_t` (e.g. `tmpl_t`).
- Cleanup macros follow the pattern `_cleanup_prefix_` (e.g.
  `_cleanup_tmpl_`).
- For refcounted objects: `prefix_ref` / `prefix_unref` / `prefix_unrefp`.
  `prefix_unref` always returns `NULL` to allow use in assignments.
  `prefix_unrefp` accepts `T **` and nullifies the pointer; it is the
  target of `_cleanup_prefix_`.
- For plain heap objects without refcounting: `prefix_free` /
  `prefix_freep`, following the same nullifying pattern.
- Destructors that clear content but leave the object allocated:
  `prefix_clear` / `prefix_done`.
- Pointer-nullifying wrappers (`prefix_unrefp`, `prefix_freep`) accept
  `T **` and set the pointer to `NULL` after release.
- Output parameters (initialized on success): prefix with `ret_`.
  Error-detail output parameters (initialized on failure): prefix with
  `reterr_`. In the parameter list, `reterr_` params come last, `ret_`
  params immediately before them.
- Command-line argument variables stored globally: prefix with `arg_`.
- No names beginning with `_` except for internal/macro use (`_cleanup_`,
  `_MAX`, `_INVALID` sentinel enum values).
- No names that shadow a C standard library function or keyword.

---

## Types

- Use `bool` from `<stdbool.h>` for boolean values. Do not use `int` for
  booleans unless interfacing with a C89 API that requires it.
- Use fixed-width types from `<stdint.h>` (`uint32_t`, `int64_t`, etc.)
  whenever the bit-width of an integer matters. Avoid `short` and `long`.
- Use `char` only for actual characters and NUL-terminated strings. Use
  `uint8_t` for raw bytes.
- Use `size_t` for sizes and counts. Use `ssize_t` for signed size values
  (e.g. `read(2)` return values).
- If a value cannot sensibly be negative, use an unsigned type. Prefer
  `unsigned` over `unsigned int`.
- Use `const` on pointer parameters that are not modified by the callee.
  Use `const` on local variables that are not reassigned. Liberal use of
  `const` is encouraged.
- Use `static` to declare all file-local functions and variables. This is
  not optional.
- Use `volatile` only for memory-mapped registers and signal-handler-shared
  globals. Never use it as a substitute for proper synchronization.
- Enumerations for flags:

  ```c
  typedef enum FooFlags {
          FOO_RDONLY  = 1 << 0,
          FOO_NOFOLLOW = 1 << 1,
  } FooFlags;
  ```

  Enumerations for modes; include a sentinel max and invalid value:

  ```c
  typedef enum FooMode {
          FOO_MODE_A,
          FOO_MODE_B,
          _FOO_MODE_MAX,
          _FOO_MODE_INVALID = -EINVAL,
  } FooMode;
  ```

  Do not explicitly set values in the middle of an enum list; either set
  all or only the first.

---

## Error Handling

- Library functions return **negative errno** values on failure
  (`-EINVAL`, `-ENOMEM`, `-EIO`, ...). Return `0` or a positive value on
  success.
- Constructors may return `NULL` on allocation failure instead of
  an `int` error code, when that is the natural return type.
- POSIX/libc calls return `-1` on error with `errno` set. Convert
  immediately:

  ```c
  fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
          return -errno;
  ```

- **Check every return value.** If a return value is intentionally
  discarded, make it explicit with a cast to `(void)`:

  ```c
  (void) close(fd);
  ```

- Library code must not log errors. Return the error code and let the
  caller decide. Debug-level logging inside library code is acceptable.
- CLI/main-program code may use `err(3)` and `warn(3)` from `<err.h>` for
  fatal errors, which are unrecoverable and call `exit`.
- Use `goto` for cleanup on error paths. Jump **forward only**, to a
  cleanup label at the end of the function. Never jump backward.

  ```c
  int
  do_work(const char *path)
  {
          int fd = -1;
          int r;

          fd = open(path, O_RDONLY | O_CLOEXEC);
          if (fd < 0) {
                  r = -errno;
                  goto finish;
          }

          r = process(fd);

  finish:
          if (fd >= 0)
                  close(fd);
          return r;
  }
  ```

  Prefer cleanup attributes over `goto` when practical (see Memory
  Management).

- Do not use `setjmp` or `longjmp`. Do not use `abort()` except as an
  unreachable-code guard.
- Public API functions must validate all pointer arguments at entry.
  Internal functions may use `assert()` for precondition checks instead.

---

## Memory Management

- Prefer **`calloc`** for allocations that require zero-initialization.
  Do not `memset` after `malloc` when `calloc` will do.
- Always check the return value of allocation functions. There is no excuse
  to skip OOM checks.
- Avoid large stack buffers. The build enforces a 2048-byte frame size limit
  via `-Wframe-larger-than=2048`. If you need more, use dynamic allocation.
- No VLAs. No `alloca`. See Build and Tooling.
- Use **cleanup attributes** for automatic resource release. This makes
  error paths correct by construction. The destructor fires at the end of
  the **enclosing C scope** (`}`), not at the end of a loop iteration.
  Never declare a cleanup variable outside a loop and expect it to release
  on each iteration -- it will only release once, on scope exit, leaking
  every iteration but the last. Instead, open a new scope inside the loop:

  ```c
  /* WRONG: t leaks on every iteration except the last */
  _cleanup_tmpl_ struct tmpl *t = NULL;
  while(condition){
          tmpl_new(&t);
          /* ... */
  }

  /* RIGHT: t is released at the closing } of each iteration */
  while(condition){
          _cleanup_tmpl_ struct tmpl *t = NULL;
          tmpl_new(&t);
          /* ... */
  }
  ```

  ```c
  int
  example(void)
  {
          autofree char *buf = NULL;

          buf = malloc(256);
          if(buf == NULL)
                  return -ENOMEM;

          /* buf is freed automatically on all return paths */
          return fill(buf, 256);
  }
  ```

  `autofree`, `autoclose`, and `autoclosefile` are defined in
  `lib/cleanup.h`. Use them for heap pointers, file descriptors, and
  `FILE *` respectively.

- Destructors must tolerate `NULL` input and treat it as a no-op,
  matching the behaviour of `free(3)`.
- Never access a pointer after freeing the referent. Use `_cleanup_` or
  nullify explicitly after free.
- `_cleanup_` has several silent failure modes to be aware of:
  - A `goto` that jumps **backward** over a cleanup variable declaration
    skips its initializer and may cause a double-free or use of garbage.
    `goto` is only safe jumping **forward** to a label after the
    declaration (the error-path pattern).
  - `longjmp` unwinds the stack without executing cleanup destructors.
    Any cleanup variable whose scope is crossed by a `longjmp` will leak.
  - Lua uses `longjmp` internally for error propagation. At any Lua/C API
    boundary inside a cleanup frame, errors must be caught with
    `lua_pcall` or equivalent protected calls before the frame exits.
    Letting a Lua error propagate out of a function with cleanup variables
    will silently skip all pending destructors.

---

## Control Flow

- **No recursion.** (JPL Rule 4 / Power of Ten Rule 1.) An acyclic call
  graph enables static stack-usage analysis. Rewrite recursive algorithms
  iteratively.
- **No `setjmp`/`longjmp`.**
- **`goto`** is permitted only for forward jumps to cleanup/error labels at
  the end of the function. Never jump backward.
- All loops that are intended to terminate must have a statically evident
  upper bound. If the bound is not obvious from the loop structure itself,
  document it with a comment or add an explicit guard assertion.
- Infinite loops: `for (;;)`, not `while (1)` or `while (true)`.
- Minimize nesting depth. Prefer early returns for guard conditions over
  deeply nested `if` blocks. Two levels of nesting is common; three is a
  sign the function should be split.
- Avoid assignments inside conditions. `if ((p = malloc(n)) == NULL)` is
  acceptable only as a well-understood idiom; introduce a variable instead
  when it aids clarity.
- No Yoda comparisons. Write `if (x == 0)`, not `if (0 == x)`. Let the
  compiler catch accidental assignments (`-Wparentheses`).
- When subtracting from a `size_t` or other unsigned type, validate that the
  result cannot wrap. `count - 1` when `count` is zero silently produces
  `SIZE_MAX`. Check before subtracting, not after.

---

## Functions

- Keep functions short. A function that does not fit on one printed page
  (approximately 60 lines) should be split. (JPL Rule 4 / Power of Ten
  Rule 4 / Barr 6.2.)
- Each function should do one thing. If you find yourself naming it
  `do_this_and_that`, split it.
- Every non-trivial public function should validate its preconditions with
  `ASSERT_RETURN`. Internal functions may use `assert()`. (JPL Rule 16.)
- Declare variables close to their first use rather than at the top of the
  function. C99 allows mixed declarations and statements; use it. `for (int
  i = 0; ...)` is preferred over a separate `int i;` at the top. Exception:
  `_cleanup_*` variables should be declared at the top of their enclosing
  scope so their lifetime and cleanup behaviour is immediately visible.
- Declare output parameters (`ret_`, `reterr_`) last in the parameter list.
- Use `const` on pointer parameters that the function does not modify.
- Do not write `foo ()` (space before the call parenthesis).
- Function pointers are a legitimate tool for encoding dispatch and protocol.
  Use them where they clarify structure; document the expected signature.
- When a function call passes `NULL` or another sentinel to mean "unset",
  annotate it:

  ```c
  foo(/* a= */ NULL, /* flags= */ 0);
  ```

---

## Preprocessor

- Limit preprocessor use to `#include` and simple object-like and
  function-like macro definitions. (JPL Rule 20 / Power of Ten Rule 8.)
- No token pasting (`##`). No variadic macros (`...`). No recursive macros.
- All macros must expand to complete syntactic units. Wrap macro bodies in
  parentheses; wrap each parameter use in parentheses.
- Prefer `static inline` functions over function-like macros. The compiler
  inlines them, they type-check, and they are debuggable. Function-like macros
  evaluate each argument at every use site; passing an expression with a side
  effect (e.g. `MIN(i++, j)`) evaluates it multiple times and produces
  incorrect results silently.
- No `#undef` of macros defined elsewhere.
- No `#define` or `#undef` inside a function or block.
- Minimize conditional compilation (`#if`, `#ifdef`). Each use should be
  justified in a comment. The standard include guard is the expected
  exception.
- Include guards use the form `PROJECTNAME_FILENAME_H`.
- No `#pragma` except where a specific platform workaround requires it,
  in which case isolate and comment it.

---

## Portability

- Target **POSIX.1-2008**. Do not use Linux-specific or BSD-specific
  interfaces in library code without a portability shim or a clear comment
  noting the constraint.
- `<err.h>` (`err`, `warn`, `errx`, `warnx`) is a BSD extension available
  on Linux (glibc) and macOS and is acceptable in CLI code. Library code
  should not use it.
- Avoid `__attribute__` extensions in public headers where possible; they
  are fine in internal and implementation code.
- Always pass `O_CLOEXEC` when opening file descriptors. File descriptors
  must not leak into child processes.
- Do not rely on the width of `int`, `long`, or pointer types. Use
  `<stdint.h>` fixed-width types when the width matters. Use `(u)intptr_t`
  for pointer-to-integer conversions.
- Floating-point formatting is locale-dependent. Use `strtod`/`snprintf`
  with `LC_NUMERIC=C` when parsing or serializing floating-point values
  portably.
- The following functions are banned; use the indicated replacements.
  `banned.h` is force-included by the build system and poisons these identifiers
  at compile time via `#pragma GCC poison`:

  | Banned | Replacement | Reason |
  |--------|-------------|--------|
  | `strcpy`, `strcat` | `strlcpy`, `strlcat` (POSIX.1-2024) | no length limit |
  | `strncpy`, `strncat` | `strlcpy`, `strlcat` | `strncpy` does not NUL-terminate; `strncat` size arg is confusing |
  | `sprintf`, `vsprintf` | `snprintf`, `vsnprintf` | no length limit |
  | `gets` | `fgets` or `getline` | no length limit |
  | `strtok` | `strtok_r` | not reentrant |
  | `atoi`, `atol`, `atoll` | `strtol`, `strtoll`, `strtoimax` | no error detection |
  | `alloca` | fixed-size array or `malloc` | unbounded stack allocation |

---

## Assertions and Defensive Coding

- Use `assert()` for **programmer errors**: violated preconditions,
  unreachable code, invariants that must hold. Do not use `assert()` for
  runtime errors (OOM, I/O failure, bad user input).
- Assertions must be **side-effect free**. The code must behave identically
  with assertions disabled (`-DNDEBUG`).
- Validate all input at public API boundaries using `ASSERT_RETURN` from
  `include/useful.h`. Unlike `assert()`, `ASSERT_RETURN` is always active and
  returns a negative errno rather than aborting — appropriate for errors a
  caller could plausibly make. Internal functions may use `assert()` instead.

  ```c
  int
  tmpl_parse(struct tmpl *t, const char *input, size_t len)
  {
          ASSERT_RETURN(t != NULL, -EINVAL);
          ASSERT_RETURN(input != NULL, -EINVAL);
          ASSERT_RETURN(len > 0, -EINVAL);
          /* ... */
  }
  ```
- Enforce resource limits on all user-controllable inputs. If the user can
  cause allocation, cap it.
- When wrapping a loop that traverses user-supplied or untrusted data,
  include an explicit iteration limit or a bound derivable from the input
  size. Unbounded loops over external data are a reliability and security
  risk. (JPL Rule 3 / Power of Ten Rule 2.)
