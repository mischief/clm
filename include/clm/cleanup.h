// SPDX-License-Identifier: ISC
/* Cleanup attribute macros for automatic resource management. */
#ifndef CLM_CLEANUP_H
#define CLM_CLEANUP_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <json-c/json.h>

static inline void
autofree_fn(void *p)
{
	if (*(void **)p)
		free(*(void **)p);
}

static inline void
autoclose_fn(int *fd)
{
	if (*fd >= 0)
		close(*fd);
}

/* Frees a NULL-terminated vector of malloc'd strings (char **), then the
 * vector itself. For returning such a vector from a function, steal it out
 * first (ret = v; v = NULL; return ret) like json_cleanup's RULE 3. */
static inline void
autofreev_fn(void *p)
{
	char **v = *(char ***)p;
	if (v == NULL)
		return;
	for (char **s = v; *s != NULL; s++)
		free(*s);
	free(v);
}

static inline void
autoclosefile_fn(FILE **fp)
{
	if (*fp != NULL)
		fclose(*fp);
}

static inline void
json_object_put_fn(struct json_object **obj)
{
	if (*obj)
		json_object_put(*obj);
}

#define autofree __attribute__((cleanup(autofree_fn)))
#define autofreev __attribute__((cleanup(autofreev_fn)))
#define autoclose __attribute__((cleanup(autoclose_fn)))
#define autoclosefile __attribute__((cleanup(autoclosefile_fn)))

/*
 * json_cleanup: frees a struct json_object* (and its whole subtree) when the
 * variable goes out of scope. Apply it to the ROOT of a json tree you build.
 *
 * ----------------------------------------------------------------------------
 * SAFE JSON CONSTRUCTION -- read this before touching json-c code.
 * ----------------------------------------------------------------------------
 *
 * THREE RULES. Follow all three and refcounting is correct by construction.
 *
 *   RULE 1 (alloc):  json_object_new_*() only fails on OOM. Check the result
 *                    of each new_* and add it to its parent immediately. Never
 *                    let a freshly-created object sit un-added across another
 *                    allocation that might early-return (that orphans it).
 *
 *   RULE 2 (steal):  json_object_object_add() and json_object_array_add()
 *                    STEAL the value reference -- they take ownership. So you
 *                    must NEVER json_object_put() something after you have
 *                    added it. The parent now owns it. Putting it = double free.
 *
 *   RULE 3 (root):   Put a json tree exactly ONCE, at its root, via
 *                    json_cleanup. On the success path you must hand the root
 *                    out without freeing it: steal it (ret = root; root = NULL)
 *                    so cleanup does not free the object you are returning.
 *
 * Borrowed objects (ones the caller still owns) must be ref'd before adding:
 *   json_object_object_add(req, "messages", json_object_get(messages));
 *
 * You do NOT need to build "inside out". Build top-down or bottom-up; the only
 * thing that matters is: check the alloc, add (which steals), never put what
 * you added, put the root once via json_cleanup, steal it out on success.
 *
 * ----------------------------------------------------------------------------
 * DO -- a builder that returns an owned object, leak-free even on OOM:
 *
 *   static struct json_object *
 *   make_prop(const char *desc)
 *   {
 *           json_cleanup struct json_object *p = json_object_new_object();
 *           struct json_object *v, *ret;
 *
 *           ASSERT_RETURN(p != NULL, NULL);
 *
 *           v = json_object_new_string("string");
 *           ASSERT_RETURN(v != NULL, NULL);   // p freed by cleanup, v is NULL: no leak
 *           json_object_object_add(p, "type", v);   // steals v; do not put v
 *
 *           v = json_object_new_string(desc);
 *           ASSERT_RETURN(v != NULL, NULL);
 *           json_object_object_add(p, "description", v);
 *
 *           ret = p;       // steal the root out so cleanup won't free it
 *           p = NULL;
 *           return ret;
 *   }
 *
 * ----------------------------------------------------------------------------
 * DON'T -- every one of these is a bug:
 *
 *   // BUG: put after add. add() already stole arr's reference; this frees a
 *   // live child, leaving a dangling pointer in the parent -> crash on use.
 *   json_object_array_add(arr, child);
 *   json_object_put(child);                 // <-- WRONG, remove this
 *
 *   // BUG: returning a json_cleanup root directly. cleanup frees it as you
 *   // return, so the caller gets a freed pointer.
 *   json_cleanup struct json_object *o = json_object_new_object();
 *   return o;                               // <-- WRONG: do ret=o; o=NULL; return ret;
 *
 *   // BUG: orphan on OOM. if the second new_* fails, the first (a, not yet
 *   // added and not under cleanup) leaks. Add each object before allocating
 *   // the next, or put both under cleanup.
 *   struct json_object *a = json_object_new_string("x");
 *   struct json_object *b = json_object_new_string("y");  // if this fails, a leaks
 *   ASSERT_RETURN(b != NULL, NULL);
 *
 *   // BUG: manual put on an error path for something already added, in
 *   // addition to json_cleanup -> double free. Let json_cleanup do it.
 * ----------------------------------------------------------------------------
 */
#define json_cleanup __attribute__((cleanup(json_object_put_fn)))

#endif
