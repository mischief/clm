// SPDX-License-Identifier: ISC
/* Cleanup attribute macros for automatic resource management. */
#ifndef CLM_CLEANUP_H
#define CLM_CLEANUP_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <cjson/cJSON.h>

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
 * first (ret = v; v = NULL; return ret) like cjson_cleanup's RULE 3. */
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
cJSON_Delete_fn(cJSON **obj)
{
	if (*obj)
		cJSON_Delete(*obj);
}

#define autofree __attribute__((cleanup(autofree_fn)))
#define autofreev __attribute__((cleanup(autofreev_fn)))
#define autoclose __attribute__((cleanup(autoclose_fn)))
#define autoclosefile __attribute__((cleanup(autoclosefile_fn)))

/*
 * json_cleanup: frees a cJSON* (and its whole subtree) when the
 * variable goes out of scope. Apply it to the ROOT of a JSON tree you build.
 *
 * ----------------------------------------------------------------------------
 * SAFE JSON CONSTRUCTION -- read this before touching cJSON code.
 * ----------------------------------------------------------------------------
 *
 * THREE RULES. Follow all three and ownership is correct by construction.
 *
 *   RULE 1 (alloc):  cJSON_Create*() only fails on OOM. Check the result
 *                    of each create and add it to its parent immediately. Never
 *                    let a freshly-created object sit un-added across another
 *                    allocation that might early-return (that orphans it).
 *
 *   RULE 2 (steal):  cJSON_AddItemToObject() and cJSON_AddItemToArray()
 *                    STEAL the value reference -- they take ownership. So you
 *                    must NEVER cJSON_Delete() something after you have
 *                    added it. The parent now owns it. Deleting it = double free.
 *
 *   RULE 3 (root):   Delete a JSON tree exactly ONCE, at its root, via
 *                    json_cleanup. On the success path you must hand the root
 *                    out without freeing it: steal it (ret = root; root = NULL)
 *                    so cleanup does not free the object you are returning.
 *
 * Borrowed objects (ones the caller still owns) must be copied before adding:
 *   cJSON_AddItemToObject(req, "messages", cJSON_Duplicate(messages, true));
 *
 * You do NOT need to build "inside out". Build top-down or bottom-up; the only
 * thing that matters is: check the alloc, add (which steals), never delete what
 * you added, delete the root once via json_cleanup, steal it out on success.
 *
 * ----------------------------------------------------------------------------
 * DO -- a builder that returns an owned object, leak-free even on OOM:
 *
 *   static cJSON *
 *   make_prop(const char *desc)
 *   {
 *           json_cleanup cJSON *p = cJSON_CreateObject();
 *           cJSON *v, *ret;
 *
 *           ASSERT_RETURN(p != NULL, NULL);
 *
 *           v = cJSON_CreateString("string");
 *           ASSERT_RETURN(v != NULL, NULL);   // p freed by cleanup, v is NULL: no leak
 *           cJSON_AddItemToObject(p, "type", v);   // steals v; do not delete v
 *
 *           v = cJSON_CreateString(desc);
 *           ASSERT_RETURN(v != NULL, NULL);
 *           cJSON_AddItemToObject(p, "description", v);
 *
 *           ret = p;       // steal the root out so cleanup won't delete it
 *           p = NULL;
 *           return ret;
 *   }
 *
 * ----------------------------------------------------------------------------
 * DON'T -- every one of these is a bug:
 *
 *   // BUG: delete after add. add() already stole arr's reference; this frees a
 *   // live child, leaving a dangling pointer in the parent -> crash on use.
 *   cJSON_AddItemToArray(arr, child);
 *   cJSON_Delete(child);                    // <-- WRONG, remove this
 *
 *   // BUG: returning a json_cleanup root directly. cleanup deletes it as you
 *   // return, so the caller gets a freed pointer.
 *   json_cleanup cJSON *o = cJSON_CreateObject();
 *   return o;                               // <-- WRONG: do ret=o; o=NULL; return ret;
 *
 *   // BUG: orphan on OOM. if the second Create* fails, the first (a, not yet
 *   // added and not under cleanup) leaks. Add each object before allocating
 *   // the next, or put both under cleanup.
 *   cJSON *a = cJSON_CreateString("x");
 *   cJSON *b = cJSON_CreateString("y");  // if this fails, a leaks
 *   ASSERT_RETURN(b != NULL, NULL);
 *
 *   // BUG: manual delete on an error path for something already added, in
 *   // addition to json_cleanup -> double free. Let json_cleanup do it.
 * ----------------------------------------------------------------------------
 */
#define json_cleanup __attribute__((cleanup(cJSON_Delete_fn)))

#endif
