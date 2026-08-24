// SPDX-License-Identifier: ISC
#include <sys/stat.h>
#include <sys/types.h>

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "xdg.h"

char *
xdg_config_path(const char *suffix)
{
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	char *out = NULL;

	if (xdg != NULL && xdg[0] != '\0') {
		size_t n = strlen(xdg) + 1 + strlen(suffix) + 1;
		out = malloc(n);
		if (out != NULL)
			(void)snprintf(out, n, "%s/%s", xdg, suffix);
	} else if (home != NULL && home[0] != '\0') {
		size_t n = strlen(home) + sizeof("/.config/") + strlen(suffix);
		out = malloc(n);
		if (out != NULL)
			(void)snprintf(out, n, "%s/.config/%s", home, suffix);
	}
	return out;
}

char *
xdg_cache_path(const char *suffix)
{
	const char *xdg = getenv("XDG_CACHE_HOME");
	const char *home = getenv("HOME");
	char *out = NULL;

	if (xdg != NULL && xdg[0] != '\0') {
		size_t n = strlen(xdg) + 1 + strlen(suffix) + 1;
		out = malloc(n);
		if (out != NULL)
			(void)snprintf(out, n, "%s/%s", xdg, suffix);
	} else if (home != NULL && home[0] != '\0') {
		size_t n = strlen(home) + sizeof("/.cache/") + strlen(suffix);
		out = malloc(n);
		if (out != NULL)
			(void)snprintf(out, n, "%s/.cache/%s", home, suffix);
	}
	return out;
}

/* mkdir every component of path, ignoring components that already exist. */
static int
mkdir_p(char *path)
{
	char *p;

	for (p = path + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(path, 0700) != 0 && errno != EEXIST) {
			*p = '/';
			return -errno;
		}
		*p = '/';
	}
	if (mkdir(path, 0700) != 0 && errno != EEXIST)
		return -errno;
	return 0;
}

char *
clm_cli_scratch_dir(const char *key)
{
	char *dir;
	char suffix[128];

	if (key == NULL || key[0] == '\0' || strchr(key, '/') != NULL)
		return NULL;
	(void)snprintf(suffix, sizeof(suffix), "clm/scratch/%s", key);
	dir = xdg_cache_path(suffix);
	if (dir == NULL)
		return NULL;
	if (mkdir_p(dir) != 0) {
		free(dir);
		return NULL;
	}
	return dir;
}

/* Remove a directory and the plain files directly inside it. */
static void
rmdir_shallow(const char *path)
{
	DIR *d = opendir(path);
	struct dirent *de;

	if (d == NULL)
		return;
	while ((de = readdir(d)) != NULL) {
		char child[1024];

		if (strcmp(de->d_name, ".") == 0 ||
		    strcmp(de->d_name, "..") == 0)
			continue;
		(void)snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
		if (unlink(child) != 0)
			rmdir_shallow(child);
	}
	(void)closedir(d);
	(void)rmdir(path);
}

size_t
clm_cli_scratch_gc(const char *const *live, size_t n, unsigned max_age_days)
{
	char *root = xdg_cache_path("clm/scratch");
	DIR *d;
	struct dirent *de;
	time_t now = time(NULL);
	size_t removed = 0, i;

	if (root == NULL)
		return 0;
	d = opendir(root);
	if (d == NULL) {
		free(root);
		return 0;
	}
	while ((de = readdir(d)) != NULL) {
		char path[1024];
		struct stat st;
		bool keep = false;

		if (de->d_name[0] == '.')
			continue;
		for (i = 0; i < n && !keep; i++)
			keep =
			    live[i] != NULL && strcmp(live[i], de->d_name) == 0;
		if (keep)
			continue;
		(void)snprintf(path, sizeof(path), "%s/%s", root, de->d_name);
		if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
			continue;
		/* A directory whose session log is gone goes regardless of
		 * age; one still young might belong to a session running in
		 * another window. */
		if (max_age_days > 0 &&
		    difftime(now, st.st_mtime) / (60 * 60 * 24) <
		        (double)max_age_days)
			continue;
		rmdir_shallow(path);
		removed++;
	}
	(void)closedir(d);
	free(root);
	return removed;
}
