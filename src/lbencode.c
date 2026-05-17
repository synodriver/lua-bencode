#include "lua.h"
#include "lauxlib.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sds.h"

#if defined(_WIN32) || defined(_WIN64)
#define DLLEXPORT __declspec(dllexport)
#else
#define DLLEXPORT
#endif /* _WIN32 */

#define LBENCODE_MAX_DEPTH 512

typedef struct
{
    const char *data;
    size_t len;
} bencode_key;

typedef enum
{
    TABLE_KIND_INVALID = -1,
    TABLE_KIND_LIST = 0,
    TABLE_KIND_DICT = 1
} table_kind;

static int
ldecode_any(lua_State *L, const char *buf, size_t bufsize, size_t *offset, int depth);

static int
lencode_any(lua_State *L, int idx, sds *r, int depth);

static int
bytes_index(const char *data, size_t datasize, int c, size_t offset, size_t *index)
{
    size_t i;

    if (offset >= datasize)
    {
        return -1;
    }

    for (i = offset; i < datasize; i++)
    {
        if ((unsigned char) data[i] == (unsigned char) c)
        {
            *index = i;
            return 0;
        }
    }

    return -1;
}

static int
parse_bencode_length(const char *buf, size_t begin, size_t end, size_t *length)
{
    size_t value = 0;
    size_t i;

    if (begin >= end)
    {
        return -1;
    }
    if (buf[begin] == '0' && begin + 1 != end)
    {
        return -1;
    }

    for (i = begin; i < end; i++)
    {
        unsigned char ch = (unsigned char) buf[i];
        size_t digit;

        if (ch < '0' || ch > '9')
        {
            return -1;
        }

        digit = (size_t) (ch - '0');
        if (value > (SIZE_MAX - digit) / 10)
        {
            return -1;
        }
        value = value * 10 + digit;
    }

    *length = value;
    return 0;
}

static int
parse_bencode_integer(const char *buf, size_t begin, size_t end, lua_Integer *integer)
{
    size_t i = begin;
    int negative = 0;
    uint64_t magnitude = 0;
    uint64_t limit;

    if (begin >= end)
    {
        return -1;
    }

    if (buf[i] == '-')
    {
        negative = 1;
        i++;
        if (i >= end)
        {
            return -1;
        }
    }

    if (buf[i] < '0' || buf[i] > '9')
    {
        return -1;
    }
    if (buf[i] == '0' && i + 1 != end)
    {
        return -1;
    }
    if (negative && buf[i] == '0')
    {
        return -1;
    }

    if (negative)
    {
        limit = (uint64_t) (-(LUA_MININTEGER + (lua_Integer) 1)) + 1U;
    }
    else
    {
        limit = (uint64_t) LUA_MAXINTEGER;
    }

    for (; i < end; i++)
    {
        unsigned char ch = (unsigned char) buf[i];
        uint64_t digit;

        if (ch < '0' || ch > '9')
        {
            return -1;
        }

        digit = (uint64_t) (ch - '0');
        if (magnitude > (limit - digit) / 10)
        {
            return -1;
        }
        magnitude = magnitude * 10 + digit;
    }

    if (negative)
    {
        if (magnitude == limit)
        {
            *integer = LUA_MININTEGER;
        }
        else
        {
            *integer = -(lua_Integer) magnitude;
        }
    }
    else
    {
        *integer = (lua_Integer) magnitude;
    }

    return 0;
}

static int
lencode_raw_string(const char *data, size_t size, sds *r)
{
    sds newsds = sdscatprintf(*r, "%zu:", size);
    if (newsds == NULL)
    {
        return -1;
    }
    *r = newsds;

    newsds = sdscatlen(*r, data, size);
    if (newsds == NULL)
    {
        return -1;
    }
    *r = newsds;
    return 0;
}

static int
compare_bencode_keys(const void *lhs, const void *rhs)
{
    const bencode_key *left = (const bencode_key *) lhs;
    const bencode_key *right = (const bencode_key *) rhs;
    size_t minlen = left->len < right->len ? left->len : right->len;
    int cmp = memcmp(left->data, right->data, minlen);

    if (cmp != 0)
    {
        return cmp;
    }
    if (left->len < right->len)
    {
        return -1;
    }
    if (left->len > right->len)
    {
        return 1;
    }
    return 0;
}

static int
ldecode_string(lua_State *L, const char *buf, size_t bufsize, size_t *offset)  /* 6:string */
{
    size_t colon;
    size_t length;

    if (!lua_checkstack(L, 1))
    {
        return -1;
    }
    if (*offset >= bufsize)
    {
        return -1;
    }
    if (bytes_index(buf, bufsize, ':', *offset, &colon) == -1)
    {
        return -1;
    }
    if (parse_bencode_length(buf, *offset, colon, &length) == -1)
    {
        return -1;
    }

    colon += 1;
    if (length > bufsize - colon)
    {
        return -1;
    }

    lua_pushlstring(L, buf + colon, length);
    *offset = colon + length;
    return 1;
}

static int
ldecode_int(lua_State *L, const char *buf, size_t bufsize, size_t *offset)  /* i-42e */
{
    size_t end;
    lua_Integer n;

    if (!lua_checkstack(L, 1))
    {
        return -1;
    }
    if (*offset >= bufsize || buf[*offset] != 'i')
    {
        return -1;
    }

    *offset += 1;
    if (bytes_index(buf, bufsize, 'e', *offset, &end) == -1)
    {
        return -1;
    }
    if (parse_bencode_integer(buf, *offset, end, &n) == -1)
    {
        return -1;
    }

    lua_pushinteger(L, n);
    *offset = end + 1;
    return 1;
}


static int
ldecode_list(lua_State *L, const char *buf, size_t bufsize, size_t *offset, int depth)
{
    if (!lua_checkstack(L, 2))
    {
        return -1;
    }
    if (*offset >= bufsize || buf[*offset] != 'l')
    {
        return -1;
    }

    *offset += 1;  /* buf[*offset]==108 */
    lua_createtable(L, 1, 0);  /* table */
    lua_Integer count = 1; /* lua array starts with 1 */
    while (1)
    {
        if (*offset >= bufsize)
        {
            return -1;
        }
        if (buf[*offset] == 'e')
        {
            break;
        }
        if (ldecode_any(L, buf, bufsize, offset, depth + 1) == -1) /*table value*/
        {
            return -1;
        }
        lua_rawseti(L, -2, count);  /* table */
        count++;
    }
    *offset += 1;
    return 1;
}

static int
ldecode_dict(lua_State *L, const char *buf, size_t bufsize, size_t *offset, int depth)
{
    if (!lua_checkstack(L, 3))
    {
        return -1;
    }
    if (*offset >= bufsize || buf[*offset] != 'd')
    {
        return -1;
    }

    *offset += 1;  /* buf[*offset]==100 */
    lua_createtable(L, 0, 1);  /* table */
    while (1)
    {
        if (*offset >= bufsize)
        {
            return -1;
        }
        if (buf[*offset] == 'e')
        {
            break;
        }
        if (ldecode_string(L, buf, bufsize, offset) == -1)  /* table  string */
        {
            return -1;
        }
        if (ldecode_any(L, buf, bufsize, offset, depth + 1) == -1)  /* table  string  value */
        {
            return -1;
        }
        lua_rawset(L, -3);  /* table */
    }
    *offset += 1;
    return 1;
}

static int
ldecode_any(lua_State *L, const char *buf, size_t bufsize, size_t *offset, int depth)
{
    if (*offset >= bufsize || depth > LBENCODE_MAX_DEPTH)
    {
        return -1;
    }

    switch (buf[*offset])
    {
        case 108: /* l */
        {
            if (ldecode_list(L, buf, bufsize, offset, depth) == -1)
            {
                return -1;
            }
            break;
        }
        case 100:
        {
            if (ldecode_dict(L, buf, bufsize, offset, depth) == -1)
            {
                return -1;
            }
            break;
        }
        case 105:
        {
            if (ldecode_int(L, buf, bufsize, offset) == -1)
            {
                return -1;
            }
            break;
        }
        default:
        {
            if (buf[*offset] >= 48 && buf[*offset] <= 57) /* 0-9 for string prefix */
            {
                if (ldecode_string(L, buf, bufsize, offset) == -1)
                {
                    return -1;
                }
            }
            else
            {
                return -1;
            }
        }
    }
    return 1;
}

static int
lloads(lua_State *L)
{
    if (lua_gettop(L) != 1)
    {
        return luaL_error(L, "loads only need 1 arg.");
    }
    size_t size, offset = 0;
    const char *buff = luaL_checklstring(L, 1, &size);
    if (ldecode_any(L, buff, size, &offset, 0) == -1)
    {
        return luaL_error(L, "not a valid bencoded string");
    }
    if (offset != size)
    {
        return luaL_error(L, "not a valid bencoded string");
    }
    return 1;
}

static int
lencode_int(lua_State *L, int idx, sds *r)
{
    sds newsds = sdscatprintf(*r, "i%" LUA_INTEGER_FMT "e", luaL_checkinteger(L, idx));
    if (newsds == NULL)
    {
        return -1;
    }
    *r = newsds;
    return 0;
}

static int
lencode_bool(lua_State *L, int idx, sds *r)
{
    int data = lua_toboolean(L, idx);
    sds newsds = sdscatprintf(*r, "i%de", data);
    if (newsds == NULL)
    {
        return -1;
    }
    *r = newsds;
    return 0;
}

static int
lencode_string(lua_State *L, int idx, sds *r)
{
    size_t size;
    const char *data = luaL_checklstring(L, idx, &size);
    return lencode_raw_string(data, size, r);
}

/* return TABLE_KIND_DICT for dict, TABLE_KIND_LIST for list, TABLE_KIND_INVALID otherwise */
static int
checktable(lua_State *L, int idx)
{
    lua_Unsigned numeric_count = 0;
    lua_Unsigned max_index = 0;
    int has_string_key = 0;
    int has_numeric_key = 0;
    int oldtop = lua_gettop(L);

    idx = lua_absindex(L, idx);
    lua_pushnil(L);
    while (lua_next(L, idx) != 0)
    {
        if (lua_type(L, -2) == LUA_TSTRING)
        {
            has_string_key = 1;
        }
        else if (lua_isinteger(L, -2))
        {
            lua_Integer key = lua_tointeger(L, -2);
            if (key <= 0)
            {
                lua_settop(L, oldtop); /* stack balance */
                return TABLE_KIND_INVALID;
            }
            has_numeric_key = 1;
            numeric_count++;
            if ((lua_Unsigned) key > max_index)
            {
                max_index = (lua_Unsigned) key;
            }
        }
        else
        {
            lua_settop(L, oldtop); /* stack balance */
            return TABLE_KIND_INVALID;
        }
        if (has_string_key && has_numeric_key)
        {
            lua_settop(L, oldtop); /* stack balance */
            return TABLE_KIND_INVALID;
        }
        lua_pop(L, 1);
    }
    lua_settop(L, oldtop); /* stack balance */

    if (has_string_key)
    {
        return TABLE_KIND_DICT;
    }
    if (!has_numeric_key)
    {
        return TABLE_KIND_LIST;
    }
    if (numeric_count != max_index)
    {
        return TABLE_KIND_INVALID;
    }
    return TABLE_KIND_LIST;
}


static int
lencode_list(lua_State *L, int idx, sds *r, int depth)
{
#ifdef DEBUG
    fprintf(stderr, "stack size before lencode_list: %d\n", lua_gettop(L));
#endif
    if (!lua_checkstack(L, 1))
    {
        return -1;
    }

    idx = lua_absindex(L, idx);

    sds newsds = sdscat(*r, "l");
    if (newsds == NULL)
    {
        return -1;
    }
    *r = newsds;
    lua_Unsigned tbsize = lua_rawlen(L, idx);
    for (lua_Unsigned i = 1; i <= tbsize; i++)
    {
        lua_rawgeti(L, idx, (lua_Integer) i);
        if (lua_isnil(L, -1) || lencode_any(L, lua_gettop(L), r, depth + 1) == -1) /*do not use negative idx here, push on stack will change that*/
        {
            lua_pop(L, 1);
            return -1;
        }
        lua_pop(L, 1);
    }
    newsds = sdscat(*r, "e");
    if (newsds == NULL)
    {
        return -1;
    }
    *r = newsds;
#ifdef DEBUG
    fprintf(stderr, "stack size after lencode_list: %d\n", lua_gettop(L));
#endif
    return 0;
}

static int
lencode_dict(lua_State *L, int idx, sds *r, int depth)
{
    bencode_key *keys = NULL;
    size_t key_count = 0;
    size_t key_capacity = 0;
    int oldtop = lua_gettop(L);

#ifdef DEBUG
    fprintf(stderr, "stack size before lencode_dict: %d\n", lua_gettop(L));
#endif
    if (!lua_checkstack(L, 2))
    {
        return -1;
    }

    idx = lua_absindex(L, idx);

    sds newsds = sdscat(*r, "d");
    if (newsds == NULL)
    {
        return -1;
    }
    *r = newsds;

    lua_pushnil(L);
    while (lua_next(L, idx) != 0)
    {
        const char *key_data;
        size_t key_len;
        bencode_key *newkeys;

        if (lua_type(L, -2) != LUA_TSTRING)
        {
            lua_settop(L, oldtop);
            free(keys);
            return -1;
        }

        key_data = lua_tolstring(L, -2, &key_len);
        if (key_count == key_capacity)
        {
            size_t new_capacity = key_capacity == 0 ? 8 : key_capacity * 2;
            newkeys = (bencode_key *) realloc(keys, new_capacity * sizeof(*keys));
            if (newkeys == NULL)
            {
                lua_settop(L, oldtop);
                free(keys);
                return -1;
            }
            keys = newkeys;
            key_capacity = new_capacity;
        }

        keys[key_count].data = key_data;
        keys[key_count].len = key_len;
        key_count++;

        lua_pop(L, 1);
    }

    qsort(keys, key_count, sizeof(*keys), compare_bencode_keys);

    for (size_t i = 0; i < key_count; i++)
    {
        if (lencode_raw_string(keys[i].data, keys[i].len, r) == -1)
        {
            free(keys);
            return -1;
        }

        lua_pushlstring(L, keys[i].data, keys[i].len);
        lua_rawget(L, idx);
        if (lencode_any(L, lua_gettop(L), r, depth + 1) == -1)
        {
            lua_pop(L, 1);
            free(keys);
            return -1;
        }
        lua_pop(L, 1);
    }

    free(keys);

    newsds = sdscat(*r, "e");
    if (newsds == NULL)
    {
        return -1;
    }
    *r = newsds;
#ifdef DEBUG
    fprintf(stderr, "stack size after lencode_dict: %d\n", lua_gettop(L));
#endif
    return 0;
}

static int
lencode_any(lua_State *L, int idx, sds *r, int depth)
{
    if (depth > LBENCODE_MAX_DEPTH)
    {
        return -1;
    }

    switch (lua_type(L, idx))
    {
        case LUA_TNUMBER:
        {
            if(lencode_int(L, idx, r)==-1)
            {
                return -1;
            }
            break;
        }
        case LUA_TBOOLEAN:
        {
            if(lencode_bool(L, idx, r)==-1)
            {
                return -1;
            }
            break;
        }
        case LUA_TSTRING:
        {
            if(lencode_string(L, idx, r)==-1)
            {
                return -1;
            }
            break;
        }
        case LUA_TTABLE:
        {
            table_kind kind = (table_kind) checktable(L, idx);

            if (kind == TABLE_KIND_DICT)
            {
                if(lencode_dict(L, idx, r, depth)==-1)
                {
                    return -1;
                }
            }
            else if (kind == TABLE_KIND_LIST)
            {
                if(lencode_list(L, idx, r, depth)==-1)
                {
                    return -1;
                }
            }
            else
            {
                return -1;
            }
            break;
        }
        default: /* todo macro for this */
        {
            return -1;
        }
    }

    return 0;
}

static int
ldumps(lua_State *L)
{
    int argc = lua_gettop(L);
    if(argc < 1 || argc > 2)
    {
        return luaL_error(L, "dumps only need data and optional bufsize");
    }
    sds ret = sdsempty();
    if(ret == NULL)
    {
        return luaL_error(L, "failed to alloc buffer");
    }
    lua_Integer bufsize = 100000;
    if(argc == 2)
    {
        bufsize = luaL_checkinteger(L, 2);
        if (bufsize < 0)
        {
            sdsfree(ret);
            return luaL_error(L, "bufsize must be non-negative");
        }
    }

    sds reserved = sdsMakeRoomFor(ret, (size_t) bufsize);
    if(reserved == NULL)
    {
        sdsfree(ret);
        return luaL_error(L, "failed to append buffer");
    }
    ret = reserved;

    if(lencode_any(L, 1, &ret, 0)==-1)
    {
        sdsfree(ret);
        return luaL_error(L, "memory error or invalid input");
    }
    lua_pushlstring(L, ret, sdslen(ret));
    sdsfree(ret);
    return 1;
}

static luaL_Reg lua_funcs[] = {
        {"loads", &lloads},
        {"dumps", &ldumps},
        {NULL, NULL}
};


DLLEXPORT int luaopen_bencode(lua_State *L)
{
    luaL_newlib(L, lua_funcs);
    return 1;
}


