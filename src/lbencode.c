#include "lua.h"
#include "lauxlib.h"
#include "string.h"

#include "util.h"
#include "sds.h"

#if defined(_WIN32) || defined(_WIN64)
#define DLLEXPORT __declspec(dllexport)
#elif
#define DLLEXPORT
#endif /* _WIN32 */

size_t bytes_index(const char *data, int c, size_t offset)
{
    char *substring = strchr(data + offset, c);
    return substring - data;
}

static int
ldecode_any(lua_State *L, const char *buf, size_t bufsize, size_t *offset);

static int
lencode_any(lua_State *L, int idx, sds *r);

static int
ldecode_string(lua_State *L, const char *buf, size_t bufsize, size_t *offset)  /* 6:string */
{
    if (!lua_checkstack(L, 1))
    {
        return -1;
    }
    size_t colon = bytes_index(buf, 58, *offset);
    if(colon >= bufsize) /* prevent from ptr cross bound */
    {
        return -1;
    }
    int64_t length = 0;
    CM_Atoi((char *) buf + *offset, (int) colon - *offset, &length);
    if (buf[offset[0]] == 48 && colon != offset[0] + 1)
    {
        return -1; /* 0 length string */
    }
    colon += 1;
    *offset = colon + length;
    lua_pushlstring(L, buf + colon, length);
    return 1;
}

static int
ldecode_int(lua_State *L, const char *buf, size_t bufsize, size_t *offset)  /* i-42e */
{
    if (!lua_checkstack(L, 1))
    {
        return -1;
    }
    *offset += 1;
    size_t end = bytes_index(buf, 101, *offset);
    if(end >= bufsize) /* prevent from ptr cross bound */
    {
        return -1;
    }
    int64_t n = 0;
    CM_Atoi((char *) buf + *offset, (int) end - *offset, &n);
    if (buf[*offset] == 45) /* - */
    {
        if (buf[*offset + 1] == 48) /* 0 */
        {
            return -1;
        }
    }
    else if (buf[*offset] == 48 && end != (*offset + 1)) /* 00000 */
    {
        return -1;
    }
    lua_pushinteger(L, n);
    *offset = end + 1;
    return 1;
}


static int
ldecode_list(lua_State *L, const char *buf, size_t bufsize, size_t *offset)
{
    if (!lua_checkstack(L, 2))
    {
        return -1;
    }
    *offset += 1;  /* buf[*offset]==108 */
    lua_createtable(L, 1, 0);  /* table */
    lua_Integer count = 1; /* lua array starts with 1 */
    while (buf[*offset] != 101)  /* e */
    {
        if (ldecode_any(L, buf, bufsize, offset) == -1) /*table value*/
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
ldecode_dict(lua_State *L, const char *buf, size_t bufsize, size_t *offset)
{
    if (!lua_checkstack(L, 3))
    {
        return -1;
    }
    *offset += 1;  /* buf[*offset]==100 */
    lua_createtable(L, 0, 1);  /* table */
    while (buf[*offset] != 101)
    {
        if (ldecode_string(L, buf, bufsize, offset) == -1)  /* table  string */
        {
            return -1;
        }
        if (ldecode_any(L, buf, bufsize, offset) == -1)  /* table  string  value */
        {
            return -1;
        }
        lua_rawset(L, -3);  /* table */
    }
    *offset += 1;
    return 1;
}

static int
ldecode_any(lua_State *L, const char *buf, size_t bufsize, size_t *offset)
{
    switch (buf[*offset])
    {
        case 108: /* l */
        {
            if (ldecode_list(L, buf, bufsize, offset) == -1)
            {
                return -1;
            }
            break;
        }
        case 100:
        {
            if (ldecode_dict(L, buf, bufsize, offset) == -1)
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
    if (ldecode_any(L, buff, size, &offset) == -1)
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
    int64_t data = (int64_t) luaL_checkinteger(L, idx);
    sds newsds = sdsMakeRoomFor(*r, 20);
    if (newsds == NULL)
    {
        return -1;
    }
    *r = newsds;
    int count = snprintf(newsds + sdslen(newsds), 20, "i%llde", data);
    sdsIncrLen(newsds, count);
    return 0;
}

static int
lencode_bool(lua_State *L, int idx, sds *r)
{
    int data = lua_toboolean(L, idx);
    sds newsds = sdsMakeRoomFor(*r, 8);
    if (newsds == NULL)
    {
        return -1;
    }
    *r = newsds;
    int count = snprintf(newsds + sdslen(newsds), 8, "i%lde", data);
    sdsIncrLen(newsds, count);
    return 0;
}

static int
lencode_string(lua_State *L, int idx, sds *r)
{
    size_t size;
    const char *data = luaL_checklstring(L, idx, &size);
    sds newsds = sdsMakeRoomFor(*r, size + 30);
    if (newsds == NULL)
    {
        return -1;
    }
    *r = newsds;
    int count;
    count = snprintf(newsds + sdslen(newsds), size + 30, "%lld:", size);
    sdsIncrLen(newsds, count);
    memcpy(newsds + sdslen(newsds), data, size);
    sdsIncrLen(newsds, (int) size);
    return 0;
}

/* return 1 for dict like, 0 for list like */
static int
checktable(lua_State *L, int idx)
{
    int oldtop = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, idx) != 0)
    {
        if (lua_type(L, -2) != LUA_TNUMBER) /* check key */
        {
            lua_settop(L, oldtop); /* stack balance */
            return 1;
        }
        lua_pop(L, 1);
    }
    lua_settop(L, oldtop); /* stack balance */
    return 0;
}


static int
lencode_list(lua_State *L, int idx, sds *r)
{
#ifdef DEBUG
    fprintf(stderr, "stack size before lencode_list: %ld\n", lua_gettop(L));
#endif
    if (!lua_checkstack(L, 1))
    {
        return -1;
    }
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
        if (lencode_any(L, lua_gettop(L), r) == -1) /*do not use negative idx here, push on stack will change that*/
        {
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
    fprintf(stderr, "stack size after lencode_list: %ld\n", lua_gettop(L));
#endif
    return 0;
}

static int
lencode_dict(lua_State *L, int idx, sds *r)
{
#ifdef DEBUG
    fprintf(stderr, "stack size before lencode_dict: %ld\n", lua_gettop(L));
#endif
    if (!lua_checkstack(L, 2))
    {
        return -1;
    }
    sds newsds = sdscat(*r, "d");
    if (newsds == NULL)
    {
        return -1;
    }
    *r = newsds;

    lua_pushnil(L);
    while (lua_next(L, idx) != 0)
    {
        if (lencode_any(L, lua_gettop(L) - 1, r) == -1) /* tb, key, val */
        {
            return -1;
        }
        if (lencode_any(L, lua_gettop(L), r) == -1)
        {
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
    fprintf(stderr, "stack size after lencode_dict: %ld\n", lua_gettop(L));
#endif
    return 0;
}

static int
lencode_any(lua_State *L, int idx, sds *r)
{
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
            if (checktable(L, idx)) /* is dict */
            {
                if(lencode_dict(L, idx, r)==-1)
                {
                    return -1;
                }
            }
            else
            {
                if(lencode_list(L, idx, r)==-1)
                {
                    return -1;
                }
            }
            break;
        }
        default: /* todo macro for this */
        {
            return luaL_error(L, "unsupported type %s", lua_typename(L, lua_type(L, idx)));
        }
    }
}

static int
ldumps(lua_State *L)
{
    if(lua_gettop(L)!=1)
    {
        return luaL_error(L, "dumps only need 1 arg.");
    }
    sds ret = sdsempty();
    if(ret==NULL)
    {
        return luaL_error(L, "unable to create sds");
    }
    if(lencode_any(L, 1, &ret)==-1)
    {
        return luaL_error(L, "memory error");
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


