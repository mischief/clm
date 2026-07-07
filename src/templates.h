// SPDX-License-Identifier: ISC
#ifndef CLM_TEMPLATES_H
#define CLM_TEMPLATES_H

/*
 * Generated at build time (see src/meson.build) by gen_templates.lua from
 * config.lua.tpl / secrets.lua.tpl -- NUL-terminated byte arrays, usable
 * anywhere a `const char *` C string is expected (e.g. write_new_file()'s
 * strlen()-based length).
 */
extern const unsigned char config_lua_tpl_data[];
extern const unsigned char secrets_lua_tpl_data[];

#endif /* CLM_TEMPLATES_H */
