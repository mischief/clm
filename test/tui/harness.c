// SPDX-License-Identifier: ISC
/*
 * Host for the Lua TUI test suite: a Lua interpreter plus the two things
 * Lua cannot do on its own -- drive a child process on a pseudo-terminal,
 * and answer its HTTP requests with canned completions.
 */
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include "mock_server.h"

/* The plugin bindings' json table, reused so tests can read session logs. */
int clm_lua_json_open(lua_State *L);

#define READ_CHUNK 65536

static struct mock_server *server;

/*
 * A pty master with no slave open reads EIO on BSD, which a poll-and-read
 * loop cannot tell from end of file. The parent keeps its own slave fd for
 * the life of the master, so the only EIO left is a real hangup.
 */
#define MAX_PTYS 64
static struct {
	int master;
	int slave;
} pty_slaves[MAX_PTYS];

static void
pty_keep_slave(int master, int slave)
{
	size_t i;

	for (i = 0; i < MAX_PTYS; i++) {
		if (pty_slaves[i].master == 0) {
			pty_slaves[i].master = master;
			pty_slaves[i].slave = slave;
			return;
		}
	}
	(void)close(slave);
}

static void
pty_drop_slave(int master)
{
	size_t i;

	for (i = 0; i < MAX_PTYS; i++) {
		if (pty_slaves[i].master == master) {
			(void)close(pty_slaves[i].slave);
			pty_slaves[i].master = 0;
			return;
		}
	}
}

/* Copy a Lua array of strings into a NULL-terminated argv. */
static char **
argv_from_table(lua_State *L, int idx)
{
	lua_Integer n = (lua_Integer)lua_rawlen(L, idx);
	char **out = calloc((size_t)n + 1, sizeof(*out));
	lua_Integer i;

	if (out == NULL)
		return NULL;
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, idx, i);
		out[i - 1] = (char *)lua_tostring(L, -1);
		lua_pop(L, 1);
	}
	return out;
}

/* Copy a Lua table of name -> value into "NAME=VALUE" strings. The strings
 * belong to the Lua stack, so the caller must keep the table alive. */
static char **
env_from_table(lua_State *L, int idx, size_t *count)
{
	char **out = NULL;
	size_t n = 0;

	lua_pushnil(L);
	while (lua_next(L, idx) != 0) {
		const char *k = lua_tostring(L, -2);
		const char *v = lua_tostring(L, -1);
		char **grown = realloc(out, (n + 1) * sizeof(*out));
		size_t len;

		if (grown == NULL || k == NULL || v == NULL) {
			lua_pop(L, 1);
			continue;
		}
		out = grown;
		len = strlen(k) + strlen(v) + 2;
		out[n] = malloc(len);
		if (out[n] != NULL) {
			(void)snprintf(out[n], len, "%s=%s", k, v);
			n++;
		}
		lua_pop(L, 1);
	}
	*count = n;
	return out;
}

static void
free_env(char **env, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		free(env[i]);
	free(env);
}

/* Open a pty pair and start argv on it. Returns the master fd and pid. */
static int
l_spawn(lua_State *L)
{
	char **argv;
	char **env;
	size_t nenv = 0;
	size_t i;
	struct winsize ws;
	int master;
	int slave;
	const char *name;
	pid_t pid;

	luaL_checktype(L, 1, LUA_TTABLE);
	luaL_checktype(L, 2, LUA_TTABLE);
	ws.ws_row = (unsigned short)luaL_checkinteger(L, 3);
	ws.ws_col = (unsigned short)luaL_checkinteger(L, 4);
	ws.ws_xpixel = 0;
	ws.ws_ypixel = 0;

	master = posix_openpt(O_RDWR | O_NOCTTY);
	if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0)
		return luaL_error(L, "openpt: %s", strerror(errno));
	name = ptsname(master);
	if (name == NULL)
		return luaL_error(L, "ptsname: %s", strerror(errno));
	argv = argv_from_table(L, 1);
	env = env_from_table(L, 2, &nenv);
	if (argv == NULL)
		return luaL_error(L, "out of memory");

	slave = open(name, O_RDWR | O_NOCTTY);
	if (slave < 0) {
		(void)close(master);
		return luaL_error(L, "open slave: %s", strerror(errno));
	}
	/* Size the line discipline once a slave exists: a master with no
	 * slave open keeps no window size on the BSDs. */
	(void)ioctl(slave, TIOCSWINSZ, &ws);

	pid = fork();
	if (pid == 0) {
		(void)setsid();
		(void)close(slave);
		slave = open(name, O_RDWR);
		if (slave < 0)
			_exit(127);
#ifdef TIOCSCTTY
		(void)ioctl(slave, TIOCSCTTY, 0);
#endif
		(void)dup2(slave, STDIN_FILENO);
		(void)dup2(slave, STDOUT_FILENO);
		(void)dup2(slave, STDERR_FILENO);
		if (slave > STDERR_FILENO)
			(void)close(slave);
		(void)close(master);
		for (i = 0; i < nenv; i++)
			(void)putenv(env[i]);
		(void)execv(argv[0], argv);
		_exit(127);
	}
	free(argv);
	free_env(env, nenv);
	if (pid < 0) {
		(void)close(slave);
		(void)close(master);
		return luaL_error(L, "fork: %s", strerror(errno));
	}
	pty_keep_slave(master, slave);
	lua_pushinteger(L, master);
	lua_pushinteger(L, pid);
	return 2;
}

/* Read whatever is ready within timeout_ms. Returns the bytes, nil on
 * timeout, or false at end of file. */
static int
l_read(lua_State *L)
{
	int fd = (int)luaL_checkinteger(L, 1);
	int timeout = (int)luaL_checkinteger(L, 2);
	char buf[READ_CHUNK];
	struct pollfd p;
	ssize_t n;

	p.fd = fd;
	p.events = POLLIN;
	p.revents = 0;
	if (poll(&p, 1, timeout) <= 0) {
		lua_pushnil(L);
		return 1;
	}
	n = read(fd, buf, sizeof(buf));
	if (n <= 0) {
		lua_pushboolean(L, 0);
		return 1;
	}
	lua_pushlstring(L, buf, (size_t)n);
	return 1;
}

static int
l_write(lua_State *L)
{
	int fd = (int)luaL_checkinteger(L, 1);
	size_t len = 0;
	const char *data = luaL_checklstring(L, 2, &len);
	size_t off = 0;

	while (off < len) {
		ssize_t n = write(fd, data + off, len - off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		off += (size_t)n;
	}
	return 0;
}

static int
l_winsize(lua_State *L)
{
	int fd = (int)luaL_checkinteger(L, 1);
	struct winsize ws;

	ws.ws_row = (unsigned short)luaL_checkinteger(L, 2);
	ws.ws_col = (unsigned short)luaL_checkinteger(L, 3);
	ws.ws_xpixel = 0;
	ws.ws_ypixel = 0;
	(void)ioctl(fd, TIOCSWINSZ, &ws);
	return 0;
}

static int
l_kill(lua_State *L)
{
	pid_t pid = (pid_t)luaL_checkinteger(L, 1);
	int sig = (int)luaL_checkinteger(L, 2);

	lua_pushboolean(L, kill(pid, sig) == 0);
	return 1;
}

/* Reap pid. With nohang, returns 0 while the child is still running. */
static int
l_wait(lua_State *L)
{
	pid_t pid = (pid_t)luaL_checkinteger(L, 1);
	int nohang = lua_toboolean(L, 2);
	int status = 0;
	pid_t got = waitpid(pid, &status, nohang ? WNOHANG : 0);

	lua_pushinteger(L, got);
	lua_pushinteger(L, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	return 2;
}

static int
l_closefd(lua_State *L)
{
	int fd = (int)luaL_checkinteger(L, 1);

	pty_drop_slave(fd);
	(void)close(fd);
	return 0;
}

static int
l_now(lua_State *L)
{
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	lua_pushnumber(L, (lua_Number)ts.tv_sec + (lua_Number)ts.tv_nsec / 1e9);
	return 1;
}

static int
l_sleep(lua_State *L)
{
	double secs = (double)luaL_checknumber(L, 1);
	struct timespec ts;

	ts.tv_sec = (time_t)secs;
	ts.tv_nsec = (long)((secs - (double)ts.tv_sec) * 1e9);
	(void)nanosleep(&ts, NULL);
	return 0;
}

static int
l_mkdtemp(lua_State *L)
{
	const char *prefix = luaL_checkstring(L, 1);
	char path[512];

	(void)snprintf(path, sizeof(path), "%sXXXXXX", prefix);
	if (mkdtemp(path) == NULL)
		return luaL_error(L, "mkdtemp: %s", strerror(errno));
	lua_pushstring(L, path);
	return 1;
}

/* Create path and any missing parent directories. */
static int
l_mkdir(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1);
	char buf[1024];
	char *p;

	(void)snprintf(buf, sizeof(buf), "%s", path);
	for (p = buf + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		(void)mkdir(buf, 0700);
		*p = '/';
	}
	lua_pushboolean(L, mkdir(buf, 0700) == 0 || errno == EEXIST);
	return 1;
}

static int
l_listdir(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1);
	DIR *d = opendir(path);
	struct dirent *e;
	int n = 0;

	lua_newtable(L);
	if (d == NULL)
		return 1;
	while ((e = readdir(d)) != NULL) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		lua_pushstring(L, e->d_name);
		lua_rawseti(L, -2, ++n);
	}
	(void)closedir(d);
	return 1;
}

static int
l_stat(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1);
	struct stat st;

	if (stat(path, &st) != 0) {
		lua_pushnil(L);
		return 1;
	}
	lua_newtable(L);
	lua_pushinteger(L, (lua_Integer)(st.st_mode & 0777));
	lua_setfield(L, -2, "mode");
	lua_pushinteger(L, (lua_Integer)st.st_mtime);
	lua_setfield(L, -2, "mtime");
	lua_pushboolean(L, S_ISDIR(st.st_mode));
	lua_setfield(L, -2, "dir");
	return 1;
}

static void
rmtree(const char *path)
{
	DIR *d = opendir(path);
	struct dirent *e;

	if (d == NULL) {
		(void)unlink(path);
		return;
	}
	while ((e = readdir(d)) != NULL) {
		char child[1024];

		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		(void)snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
		rmtree(child);
	}
	(void)closedir(d);
	(void)rmdir(path);
}

static int
l_rmtree(lua_State *L)
{
	rmtree(luaL_checkstring(L, 1));
	return 0;
}

static int
l_setenv(lua_State *L)
{
	(void)setenv(luaL_checkstring(L, 1), luaL_checkstring(L, 2), 1);
	return 0;
}

/* Run argv to completion with its output captured. Returns the exit status
 * and everything it wrote to stdout. */
static int
l_run(lua_State *L)
{
	char **argv;
	char **env;
	size_t nenv = 0;
	size_t i;
	int fds[2];
	luaL_Buffer out;
	pid_t pid;
	int status = 0;

	luaL_checktype(L, 1, LUA_TTABLE);
	luaL_checktype(L, 2, LUA_TTABLE);
	argv = argv_from_table(L, 1);
	env = env_from_table(L, 2, &nenv);
	if (argv == NULL || pipe(fds) != 0) {
		free(argv);
		free_env(env, nenv);
		return luaL_error(L, "run: %s", strerror(errno));
	}
	pid = fork();
	if (pid == 0) {
		(void)close(fds[0]);
		(void)dup2(fds[1], STDOUT_FILENO);
		if (fds[1] > STDERR_FILENO)
			(void)close(fds[1]);
		for (i = 0; i < nenv; i++)
			(void)putenv(env[i]);
		(void)execv(argv[0], argv);
		_exit(127);
	}
	free(argv);
	free_env(env, nenv);
	(void)close(fds[1]);
	luaL_buffinit(L, &out);
	for (;;) {
		char buf[4096];
		ssize_t n = read(fds[0], buf, sizeof(buf));

		if (n <= 0)
			break;
		luaL_addlstring(&out, buf, (size_t)n);
	}
	(void)close(fds[0]);
	(void)waitpid(pid, &status, 0);
	/* The buffer owns the stack top until it is finished, so the exit
	 * status can only be pushed once the string is complete. */
	luaL_pushresult(&out);
	lua_pushinteger(L, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	lua_insert(L, -2);
	return 2;
}

static const luaL_Reg sys_funcs[] = {
    {"spawn", l_spawn},
    {"read", l_read},
    {"write", l_write},
    {"winsize", l_winsize},
    {"kill", l_kill},
    {"wait", l_wait},
    {"close", l_closefd},
    {"now", l_now},
    {"sleep", l_sleep},
    {"mkdtemp", l_mkdtemp},
    {"mkdir", l_mkdir},
    {"listdir", l_listdir},
    {"stat", l_stat},
    {"rmtree", l_rmtree},
    {"setenv", l_setenv},
    {"run", l_run},
    {NULL, NULL},
};

static int
open_sys(lua_State *L)
{
	luaL_newlib(L, sys_funcs);
	lua_pushinteger(L, SIGTERM);
	lua_setfield(L, -2, "SIGTERM");
	lua_pushinteger(L, SIGWINCH);
	lua_setfield(L, -2, "SIGWINCH");
	return 1;
}

static int
l_mock_start(lua_State *L)
{
	if (server == NULL) {
		server = mock_start();
		if (server == NULL)
			return luaL_error(L, "mock server failed to start");
	}
	lua_pushstring(L, mock_url(server));
	return 1;
}

static int
l_mock_scratch(lua_State *L)
{
	lua_pushstring(L, server != NULL ? mock_scratch(server) : "");
	return 1;
}

static int
l_mock_many_calls(lua_State *L)
{
	lua_pushinteger(L, mock_many_calls());
	return 1;
}

static int
l_mock_request_log(lua_State *L)
{
	if (server != NULL)
		mock_request_log(server, luaL_optstring(L, 1, NULL));
	return 0;
}

static int
l_mock_stop(lua_State *L)
{
	(void)L;
	mock_stop(server);
	server = NULL;
	return 0;
}

static const luaL_Reg mock_funcs[] = {
    {"start", l_mock_start},
    {"scratch", l_mock_scratch},
    {"many_calls", l_mock_many_calls},
    {"request_log", l_mock_request_log},
    {"stop", l_mock_stop},
    {NULL, NULL},
};

static int
open_mock(lua_State *L)
{
	luaL_newlib(L, mock_funcs);
	return 1;
}

int
main(int argc, char **argv)
{
	lua_State *L;
	int rc = 0;
	int i;

	if (argc < 2) {
		(void)fprintf(stderr, "usage: %s script.lua [args]\n", argv[0]);
		return 2;
	}
	/* A client that goes away mid-stream must not kill the harness. */
	(void)signal(SIGPIPE, SIG_IGN);

	L = luaL_newstate();
	if (L == NULL)
		return 1;
	luaL_openlibs(L);
	luaL_requiref(L, "sys", open_sys, 0);
	lua_pop(L, 1);
	luaL_requiref(L, "mock", open_mock, 0);
	lua_pop(L, 1);
	(void)clm_lua_json_open(L);

	lua_newtable(L);
	for (i = 1; i < argc; i++) {
		lua_pushstring(L, argv[i]);
		lua_rawseti(L, -2, i - 1);
	}
	lua_setglobal(L, "arg");

	if (luaL_loadfile(L, argv[1]) != LUA_OK ||
	    lua_pcall(L, 0, 1, 0) != LUA_OK) {
		(void)fprintf(stderr, "%s\n", lua_tostring(L, -1));
		rc = 1;
	} else if (lua_isinteger(L, -1)) {
		rc = (int)lua_tointeger(L, -1);
	}
	lua_close(L);
	mock_stop(server);
	return rc;
}
