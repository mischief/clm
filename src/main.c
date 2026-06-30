// SPDX-License-Identifier: ISC
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clm/clm.h"

static void
usage(const char *prog)
{
	fprintf(stderr, "usage: %s [--oneshot PROMPT] [--url URL] [--model NAME]\n", prog);
}

int
main(int argc, char *argv[])
{
	const char *api_url = "http://127.0.0.1:8081/v1/chat/completions";
	const char *model = "local-model";
	char *oneshot = NULL;
	struct clm_cfg cfg = {0};
	_cleanup_clm_ struct clm_agent *agent = NULL;
	int opt, r;

	const struct option opts[] = {
		{"oneshot", required_argument, NULL, 'o'},
		{"url", required_argument, NULL, 'u'},
		{"model", required_argument, NULL, 'm'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0},
	};

	while ((opt = getopt_long(argc, argv, "o:u:m:h", opts, NULL)) != -1) {
		switch (opt) {
		case 'o': oneshot = optarg; break;
		case 'u': api_url = optarg; break;
		case 'm': model = optarg; break;
		case 'h': usage(argv[0]); return 0;
		default: usage(argv[0]); return 1;
		}
	}

	cfg.api_key = "sk-no-key-required";
	cfg.base_url = api_url;
	cfg.provider = CLM_PROVIDER_OPENAI;
	cfg.model = model;
	cfg.max_iterations = 25;

	r = clm_agent_new(&cfg, &agent);
	if (r < 0) {
		fprintf(stderr, "error: failed to create agent (%d)\n", r);
		return 1;
	}

	if (oneshot != NULL) {
		char *result = NULL;
		r = clm_agent_run(agent, oneshot, &result);
		if (r < 0) {
			fprintf(stderr, "error: %s\n", clm_agent_get_last_error(agent));
			return 1;
		}
		printf("%s\n", result);
		free(result);
		return 0;
	}

	printf("clm agent. api: %s\n", api_url);
	printf("type 'quit' or 'exit' to stop.\n\n");

	{
		char *line = NULL;
		size_t cap = 0;
		ssize_t n;

		for (;;) {
			printf("you> ");
			fflush(stdout);

			n = getline(&line, &cap, stdin);
			if (n <= 0)
				break;
			if (line[n - 1] == '\n')
				line[n - 1] = '\0';
			if (line[0] == '\0')
				continue;
			if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0)
				break;

			char *result = NULL;
			r = clm_agent_run(agent, line, &result);
			if (r < 0) {
				fprintf(stderr, "error: %s\n", clm_agent_get_last_error(agent));
				continue;
			}
			printf("assistant> %s\n\n", result);
			free(result);
		}
		free(line);
	}

	return 0;
}
