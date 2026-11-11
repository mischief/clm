// SPDX-License-Identifier: ISC
/*
 * Subscription OAuth (Claude Code-style PKCE) login helpers for the clm
 * binary. Not installed; used by `clm oauth login` and, later, the
 * Anthropic provider's automatic refresh.
 *
 * These implement the authorization-code + PKCE flow that the Claude Code
 * CLI uses to obtain a plan-pool (subscription) token:
 *
 *   1. generate a code_verifier and its S256 code_challenge,
 *   2. print the authorize URL (Anthropic's own client_id + server-side
 *      callback -- no clm registration needed),
 *   3. after the user pastes back the authorization code, exchange it at
 *      the token endpoint for a short-lived access token and a rotating
 *      refresh token,
 *   4. persist the pair in clm's own store (never touching ~/.claude/).
 */
#ifndef CLM_OAUTH_H
#define CLM_OAUTH_H

/* Anthropic's Claude Code OAuth client. */
#define CLM_ANTHROPIC_OAUTH_CLIENT_ID "9d1c250a-e61b-44d9-88ed-5944d1962f5e"

/* Where the authorization code is pasted (server-side callback). */
#define CLM_ANTHROPIC_OAUTH_REDIRECT                                           \
	"https://platform.claude.com/oauth/code/callback"

/* The token endpoint that serves this credential family (verified live). */
#define CLM_ANTHROPIC_OAUTH_TOKEN_URL "https://api.anthropic.com/v1/oauth/token"

/* Default scope set for a Claude Code-style login. */
#define CLM_ANTHROPIC_OAUTH_SCOPE                                              \
	"org:create_api_key user:profile user:inference "                      \
	"user:sessions:claude_code user:mcp_servers user:file_upload "         \
	"user:plugins"

#define CLM_OAUTH_VERIFIER_LEN 64 /* bytes, before base64url; >43 -> OK */

/*
 * Generate a PKCE code_verifier (random, base64url) and its S256
 * code_challenge. Both are NUL-terminated. Returns 0 on success.
 */
int clm_oauth_pkce(char *verifier, size_t vlen, char *challenge, size_t clen);

/*
 * Exchange an authorization code for tokens. The `code` is what the user
 * pasted back from the callback; `verifier` is the PKCE code_verifier used
 * to build the challenge in the authorize URL. On success fills access_out,
 * refresh_out (malloc'd), *expires_in and *refresh_expires_in (seconds),
 * and *scope_out (may be NULL). Returns 0 on success, -1 on network/HTTP
 * error, -2 on a non-200 response (with *err_body set if non-NULL).
 */
int clm_oauth_exchange_code(const char *client_id, const char *code,
    const char *redirect_uri, const char *verifier, char **access_out,
    char **refresh_out, long *expires_in, long *refresh_expires_in,
    char **scope_out, char **err_body);

/*
 * Refresh an access token. Same interface as clm_oauth_exchange_code but
 * takes the current refresh_token and the scope list (space-joined);
 * refresh_token is rotated on success.
 */
int clm_oauth_refresh(const char *client_id, const char *refresh_token,
    const char *scope, char **access_out, char **refresh_out, long *expires_in,
    long *refresh_expires_in, char **scope_out, char **err_body);

/* A clm-owned subscription token store (oauth-anthropic.json). */
struct clm_oauth_store {
	char *client_id;
	char *access_token;
	char *refresh_token;
	long expires_at;         /* ms since epoch */
	long refresh_expires_at; /* ms since epoch */
	char *scope;             /* space-joined */
};

/*
 * Load a token store from `path` (JSON). Fills *out. Returns 0 on success,
 * -1 on error (missing file, bad JSON). Caller frees via
 * clm_oauth_store_free().
 */
int clm_oauth_store_load(const char *path, struct clm_oauth_store *out);

/* Free the fields of a store loaded by clm_oauth_store_load(). */
void clm_oauth_store_free(struct clm_oauth_store *s);

/*
 * Atomically write a token store to `path` (0600). `expires_in` and
 * `refresh_expires_in` are seconds from now (the token endpoint's units);
 * they are stored as absolute ms epochs. Returns 0 on success.
 */
int clm_oauth_store_save(const char *path, const char *client_id,
    const char *access, const char *refresh, long expires_in,
    long refresh_expires_in, const char *scope);

/*
 * Resolve a fresh access token for a store: if the stored access token is
 * still valid (with a small safety buffer) it is returned as-is; otherwise
 * the refresh token is used to mint a new one and the store is rewritten.
 * *access_out is malloc'd (caller frees). *err, if non-NULL, receives a
 * malloc'd message on failure. Returns 0 on success.
 */
int clm_oauth_resolve(
    const char *path, const char *client_id, char **access_out, char **err);

/* Expand a leading "~/" in a config-supplied path to $HOME. Returns a
 * malloc'd string. */
char *clm_oauth_expand(const char *p);

#endif /* CLM_OAUTH_H */
