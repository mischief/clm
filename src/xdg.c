// SPDX-License-Identifier: ISC
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
