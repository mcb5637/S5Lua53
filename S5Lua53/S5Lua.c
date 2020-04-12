#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include <string.h>
#include <stdlib.h>

//same as 53 #define lua5_minstack 20
#define __LUA_REGISTRYINDEX	(-10000)
#define __LUA_GLOBALSINDEX (-10001)
#define __LUA_ENVIRONINDEX (-10002)
#define __lua_upvalueindex(i)	(__LUA_ENVIRONINDEX - (i))

// lua type constants equal

#define __isregindex(i) (i==__LUA_REGISTRYINDEX)
#define __isglobalsindex(i) (i==__LUA_GLOBALSINDEX)
#define __isenviroindex(i) (i==__LUA_ENVIRONINDEX)
#define __isupvalIndex(i) (i<__LUA_ENVIRONINDEX)

// x = __LUA_ENVIRONINDEX - i | +i-x
// i = __LUA_ENVIRONINDEX - x

int __getupvalindex(i) {
	i = __LUA_ENVIRONINDEX - i;
	return lua_upvalueindex(i);
}

typedef struct __lua_Debug {
	int event;
	const char* name;      /* (n) */
	const char* namewhat;  /* (n) `global', `local', `field', `method' */
	const char* what;      /* (S) `Lua' function, `C' function, Lua `main' */
	const char* source;    /* (S) */
	int currentline;       /* (l) */
	int nups;              /* (u) number of upvalues */
	int linedefined;       /* (S) */
	char short_src[LUA_IDSIZE]; /* (S) */

	/* private part */
	void* i_ci;  /* active function */ // this was an int, but the pointer means less conversions (and is the same in asm)
} __lua_Debug;

void __zeroDbg(__lua_Debug* d) {
	d->currentline = 0;
	d->event = 0;
	d->i_ci = 0;
	d->linedefined = 0;
	d->name = 0;
	d->namewhat = 0;
	d->nups = 0;
	for (int i=0; i<LUA_IDSIZE; i++)
		d->short_src[i] = 0;
	d->source = 0;
	d->what = 0;
}

void __zeroDbgIn(lua_Debug* d) {
	d->currentline = 0;
	d->event = 0;
	d->i_ci = 0;
	d->linedefined = 0;
	d->name = 0;
	d->namewhat = 0;
	d->nups = 0;
	for (int i = 0; i < LUA_IDSIZE; i++)
		d->short_src[i] = 0;
	d->source = 0;
	d->what = 0;
	d->istailcall = 0;
	d->isvararg = 0;
	d->lastlinedefined = 0;
	d->nparams = 0;
}

void __dbgCopyOut(__lua_Debug* d, const lua_Debug* inter) {
	d->currentline = inter->currentline;
	d->event = inter->event;
	d->i_ci = inter->i_ci;
	d->linedefined = inter->linedefined;
	d->name = inter->name;
	d->namewhat = inter->namewhat;
	d->nups = inter->nups;
	//d->short_src = inter->short_src;
	memcpy(&(d->short_src), &(inter->short_src), sizeof(char) * LUA_IDSIZE);
	d->short_src[LUA_IDSIZE - 1] = 0; // make sure last char is 0
	d->source = inter->source;
	d->what = inter->what;
}

void __dbgCopyIn(const __lua_Debug* d, lua_Debug* inter) {
	inter->currentline = d->currentline;
	inter->event = d->event;
	inter->i_ci = d->i_ci;
	inter->linedefined = d->linedefined;
	inter->name = d->name;
	inter->namewhat = d->namewhat;
	inter->nups = d->nups;
	//inter->short_src = d->short_src;
	memcpy(&(inter->short_src), &(d->short_src), sizeof(char) * LUA_IDSIZE);
	inter->short_src[LUA_IDSIZE - 1] = 0; // make sure last char is 0
	inter->source = d->source;
	inter->what = d->what;
}

typedef void (*__lua_Hook) (lua_State* L, __lua_Debug* ar);

typedef struct __lua_adddata {
	__lua_Hook hook;
	void* userdata;
	int bb_userstate;
} __lua_adddata;

typedef const char* (*__lua_Chunkreader) (lua_State* L, void* data, size_t* size);
struct __chunkreaderData {
	void* data;
	__lua_Chunkreader reader;
	int eof;
};

typedef int (*__lua_Chunkwriter) (lua_State* L, const void* p, size_t sz, void* ud); // the same as lua_Writer
struct __chunkwriterData {
	void* data;
	__lua_Chunkwriter writer;
};

typedef struct __luaL_Buffer {
	char* p;                      /* current position in buffer */
	int lvl;  /* number of strings in the stack (level) */
	lua_State* L;
	//char buffer[LUAL_BUFFERSIZE]; // buffersize changed so i have to malloc another buffer
	luaL_Buffer* realbuff;
} __luaL_Buffer;

char* __onopenluaexec = ""
"string.gfind = string.gmatch\n"
"function table.setn(t, n) t[n+1] = nil end\n"
"function table.getn(t) return #t end\n"
"math.mod = math.fmod\n"
"function table.foreach(t, f) for k,v in pairs(t) do local r = f(k,v) if r~=nil then return r end end end\n"
"function table.foreachi(t, f) for k,v in ipairs(t) do local r = f(k,v) if r~=nil then return r end end end\n"
"unpack = table.unpack\n"
"function math.pow(a,b) return a^b end\n"
"math.atan = math.tan\n"
"function math.ldexp(x, e) return x*2^e end\n"
"os_time = os.time\n"
"os_date = os.date\n"
"os_difftime = os.difftime\n"
"os_clock = os.clock\n"
"io = nil\n"
"os = nil\n"
"require = nil\n"
"dofile = nil\n"
"loadfile = nil\n"
"package = nil\n"
"";

int __luaOpenLibs(lua_State* L) {
	luaL_openlibs(L); // do it here so i dont have to create all the small funcs with requiref (also this variant loads debug)
	int r = luaL_dostring(L, __onopenluaexec);
	if (r != LUA_OK)
		lua_setglobal(L, "errmsg");
	//lua_pushcfunction(L, &__luaOpenLibs);
	//lua_setglobal(L, "ReloadLibs");
	return 0;
}

// state handling

lua_State* __lua_open(void) {
	lua_State* L = luaL_newstate();
	void** e = lua_getextraspace(L);
	void* m = malloc(sizeof(__lua_adddata));
	*e = m;
	__luaOpenLibs(L);
	return L;
}

void __lua_close(lua_State* L) {
	void** e = lua_getextraspace(L);
	free(*e);
	lua_close(L);
}

//stack

int __lua_gettop(lua_State* L) {
	return lua_gettop(L);
}

int __lua_checkstack(lua_State* L, int extra) {
	return lua_checkstack(L, extra);
}

// stack manipulation

void __lua_settop(lua_State* L, int index) {
	lua_settop(L, index);
}

void __lua_pushvalue(lua_State* L, int index) {
	if (__isglobalsindex(index) || __isenviroindex(index)) {
		lua_pushglobaltable(L);
		return;
	}
	else if (__isregindex(index)) {
		index = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index)) {
		index = __getupvalindex(index);
	}
	lua_pushvalue(L, index);
}

void __lua_remove(lua_State* L, int index) {
	lua_remove(L, index);
}

void __lua_insert(lua_State* L, int index) {
	lua_insert(L, index);
}

void __lua_replace(lua_State* L, int index) {
	lua_replace(L, index);
}

//query stack

int __lua_type(lua_State* L, int index) {
	if (__isregindex(index)) {
		index = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index)) {
		index = __getupvalindex(index);
	}
	return lua_type(L, index);
}

int __lua_isnil(lua_State* L, int index) {
	if (__isregindex(index)) {
		index = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index)) {
		index = __getupvalindex(index);
	}
	return lua_isnil(L, index);
}

int __lua_isboolean(lua_State* L, int index) {
	if (__isregindex(index)) {
		index = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index)) {
		index = __getupvalindex(index);
	}
	return lua_isboolean(L, index);
}

int __lua_isnumber(lua_State* L, int index) {
	if (__isregindex(index)) {
		index = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index)) {
		index = __getupvalindex(index);
	}
	return lua_isnumber(L, index);
}

int __lua_isstring(lua_State* L, int index) {
	if (__isregindex(index)) {
		index = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index)) {
		index = __getupvalindex(index);
	}
	return lua_isstring(L, index);
}

int __lua_istable(lua_State* L, int index) {
	if (__isregindex(index)) {
		index = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index)) {
		index = __getupvalindex(index);
	}
	return lua_istable(L, index);
}

int __lua_isfunction(lua_State* L, int index) {
	if (__isregindex(index)) {
		index = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index)) {
		index = __getupvalindex(index);
	}
	return lua_isfunction(L, index);
}

int __lua_iscfunction(lua_State* L, int index) {
	if (__isregindex(index)) {
		index = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index)) {
		index = __getupvalindex(index);
	}
	return lua_iscfunction(L, index);
}

int __lua_isuserdata(lua_State* L, int index) {
	if (__isregindex(index)) {
		index = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index)) {
		index = __getupvalindex(index);
	}
	return lua_isuserdata(L, index);
}

int __lua_islightuserdata(lua_State* L, int index) {
	if (__isregindex(index)) {
		index = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index)) {
		index = __getupvalindex(index);
	}
	return lua_islightuserdata(L, index);
}

const char* __lua_typename(lua_State* L, int type) {
	return lua_typename(L, type);
}

// stack compare

int __lua_equal(lua_State* L, int index1, int index2) {
	if (__isregindex(index1)) {
		index1 = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index1)) {
		index1 = __getupvalindex(index1);
	}
	if (__isregindex(index2)) {
		index2 = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index2)) {
		index2 = __getupvalindex(index2);
	}
	return lua_compare(L, index1, index2, LUA_OPEQ);
}

int __lua_rawequal(lua_State* L, int index1, int index2) {
	if (__isregindex(index1)) {
		index1 = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index1)) {
		index1 = __getupvalindex(index1);
	}
	if (__isregindex(index2)) {
		index2 = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index2)) {
		index2 = __getupvalindex(index2);
	}
	return lua_rawequal(L, index1, index2);
}

int __lua_lessthan(lua_State* L, int index1, int index2) {
	if (__isregindex(index1)) {
		index1 = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index1)) {
		index1 = __getupvalindex(index1);
	}
	if (__isregindex(index2)) {
		index2 = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index2)) {
		index2 = __getupvalindex(index2);
	}
	return lua_compare(L, index1, index2, LUA_OPLT);
}

// get from stack

int __lua_toboolean(lua_State* L, int index) {
	if (__isregindex(index)) {
		index = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index)) {
		index = __getupvalindex(index);
	}
	return lua_toboolean(L, index);
}

lua_Number __lua_tonumber(lua_State* L, int index) {
	if (__isregindex(index)) {
		index = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index)) {
		index = __getupvalindex(index);
	}
	return lua_tonumber(L, index);
}

const char* __lua_tostring(lua_State* L, int index) {
	if (__isregindex(index)) {
		index = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index)) {
		index = __getupvalindex(index);
	}
	return lua_tolstring(L, index, NULL);
}

size_t __lua_strlen(lua_State* L, int index) {
	if (__isregindex(index)) {
		index = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index)) {
		index = __getupvalindex(index);
	}
	size_t r = 0;
	lua_tolstring(L, index, &r);
	return r;
}

lua_CFunction __lua_tocfunction(lua_State* L, int index) {
	if (__isregindex(index)) {
		index = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index)) {
		index = __getupvalindex(index);
	}
	return lua_tocfunction(L, index);
}

void* __lua_touserdata(lua_State* L, int index) {
	if (__isregindex(index)) {
		index = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index)) {
		index = __getupvalindex(index);
	}
	return lua_touserdata(L, index);
}

lua_State* __lua_tothread(lua_State* L, int index) {
	if (__isregindex(index)) {
		index = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index)) {
		index = __getupvalindex(index);
	}
	return lua_tothread(L, index);
}

const void* __lua_topointer(lua_State* L, int index) {
	if (__isregindex(index)) {
		index = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index)) {
		index = __getupvalindex(index);
	}
	return lua_topointer(L, index);
}

// push to stack

void __lua_pushboolean(lua_State* L, int b) {
	lua_pushboolean(L, b);
}

void __lua_pushnumber(lua_State* L, lua_Number n) {
	lua_pushnumber(L, n);
}

void __lua_pushlstring(lua_State* L, const char* s, size_t len) {
	lua_pushlstring(L, s, len);
}

void __lua_pushstring(lua_State* L, const char* s) {
	lua_pushstring(L, s);
}

void __lua_pushnil(lua_State* L) {
	lua_pushnil(L);
}

void __lua_pushcfunction(lua_State* L, lua_CFunction f) {
	lua_pushcfunction(L, f);
}

void __lua_pushlightuserdata(lua_State* L, void* p) {
	lua_pushlightuserdata(L, p);
}

const char* __lua_pushfstring(lua_State* L, const char* fmt, ...) {
	va_list argp;
	va_start(argp, fmt);
	const char* r = lua_pushvfstring(L, fmt, argp);
	va_end(argp);
	return r;
}

const char* __lua_pushvfstring(lua_State* L, const char* fmt, va_list argp) {
	return lua_pushvfstring(L, fmt, argp);
}

// concat (what does it have to do with push ??)

void __lua_concat(lua_State* L, int n) {
	lua_concat(L, n);
}

// gc

int __lua_getgccount(lua_State* L) {
	return lua_gc(L, LUA_GCCOUNT, 0);
}

int __lua_getgcthreshold(lua_State* L) { // no longer available in 5.3
	return lua_gc(L, LUA_GCCOUNT, 0) * 8;
}

void __lua_setgcthreshold(lua_State* L, int newthreshold) { // also not available in 5.3, so just collecting garbage
	lua_gc(L, LUA_GCCOLLECT, 0);
}

// userdata

void* __lua_newuserdata(lua_State* L, size_t size) {
	return lua_newuserdata(L, size);
}

// metatables

int __lua_getmetatable(lua_State* L, int index) {
	if (__isregindex(index)) {
		index = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index)) {
		index = __getupvalindex(index);
	}
	return lua_getmetatable(L, index);
}

int __lua_setmetatable(lua_State* L, int index) {
	if (__isregindex(index)) {
		index = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index)) {
		index = __getupvalindex(index);
	}
	return lua_setmetatable(L, index);
}

// load lua chunks

const char* __luaReader(lua_State* L, void* data, size_t* size) {
	struct __chunkreaderData* d = data;
	const char* buff = d->reader(L, d->data, size);
	if (buff == NULL || *size == 0) {
		buff = NULL;
		*size = 0;
	}
	return buff;
}

int __lua_load(lua_State* L, __lua_Chunkreader reader, void* data, const char* chunkname) { // savegame loading
	struct __chunkreaderData d;
	d.data = data;
	d.reader = reader;
	d.eof = 0;
	int r = lua_load(L, &__luaReader, &d, chunkname, NULL);
	return r;
}

// table man

void __lua_newtable(lua_State* L) {
	lua_newtable(L);
}

void __lua_gettable(lua_State* L, int index) {
	if (__isglobalsindex(index) || __isenviroindex(index)) {
		lua_pushglobaltable(L);
		lua_insert(L, 1);
		lua_gettable(L, 1);
		lua_remove(L, 1);
		return;
	}
	else if (__isregindex(index)) {
		index = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index)) {
		index = __getupvalindex(index);
	}
	lua_gettable(L, index);
}

void __lua_rawget(lua_State* L, int index) {
	if (__isglobalsindex(index) || __isenviroindex(index)) {
		lua_pushglobaltable(L);
		lua_insert(L, 1);
		lua_rawget(L, 1);
		lua_remove(L, 1);
		return;
	}
	else if (__isregindex(index)) {
		index = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index)) {
		index = __getupvalindex(index);
	}
	lua_rawget(L, index);
}

void __lua_settable(lua_State* L, int index) {
	if (__isglobalsindex(index) || __isenviroindex(index)) {
		lua_pushglobaltable(L);
		lua_insert(L, 1);
		lua_settable(L, 1);
		lua_remove(L, 1);
		return;
	}
	else if (__isregindex(index)) {
		index = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index)) {
		index = __getupvalindex(index);
	}
	lua_settable(L, index);
}

void __lua_rawset(lua_State* L, int index) {
	if (__isglobalsindex(index) || __isenviroindex(index)) {
		lua_pushglobaltable(L);
		lua_insert(L, 1);
		lua_rawset(L, 1);
		lua_remove(L, 1);
		return;
	}
	else if (__isregindex(index)) {
		index = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index)) {
		index = __getupvalindex(index);
	}
	lua_rawset(L, index);
}

int __lua_next(lua_State* L, int index) {
	if (__isglobalsindex(index) || __isenviroindex(index)) {
		lua_pushglobaltable(L);
		lua_insert(L, 1);
		int r = lua_next(L, 1);
		lua_remove(L, 1);
		return r;
	}
	else if (__isregindex(index)) {
		index = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index)) {
		index = __getupvalindex(index);
	}
	int r = lua_next(L, index);
	return r;
}

// environments (dont exist anymore, always use globals)

void __lua_getfenv(lua_State* L, int index) {
	lua_pushglobaltable(L);
}

int  __lua_setfenv(lua_State* L, int index) {
	return 0;
}

// table i

void __lua_rawgeti(lua_State* L, int index, int n) {
	if (__isglobalsindex(index) || __isenviroindex(index)) {
		lua_pushglobaltable(L);
		lua_insert(L, 1);
		lua_rawgeti(L, 1, n);
		lua_remove(L, 1);
		return;
	}
	else if (__isregindex(index)) {
		index = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index)) {
		index = __getupvalindex(index);
	}
	lua_rawgeti(L, index, n);
}

void __lua_rawseti(lua_State* L, int index, int n) {
	if (__isglobalsindex(index) || __isenviroindex(index)) {
		lua_pushglobaltable(L);
		lua_insert(L, 1);
		lua_rawseti(L, 1, n);
		lua_remove(L, 1);
		return;
	}
	else if (__isregindex(index)) {
		index = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(index)) {
		index = __getupvalindex(index);
	}
	lua_rawseti(L, index, n);
}

// calls

void __lua_call(lua_State* L, int nargs, int nresults) {
	lua_call(L, nargs, nresults);
}

int __lua_pcall(lua_State* L, int nargs, int nresults, int errfunc) {
	if (__isregindex(errfunc)) {
		errfunc = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(errfunc)) {
		errfunc = __getupvalindex(errfunc);
	}
	return lua_pcall(L, nargs, nresults, errfunc);
}

// c closure

void __lua_pushcclosure(lua_State* L, lua_CFunction fn, int n) {
	lua_pushcclosure(L, fn, n);
}

// error handling

lua_CFunction __lua_atpanic(lua_State* L, lua_CFunction panicf) {
	return lua_atpanic(L, panicf);
}

int __lua_cpcall(lua_State* L, lua_CFunction func, void* ud) {
	lua_pushcfunction(L, func);
	lua_pushlightuserdata(L, ud);
	return lua_pcall(L, 1, 0, 0);
}

void __lua_error(lua_State* L) {
	lua_error(L);
}

// threads

lua_State* __lua_newthread(lua_State* L) {
	return lua_newthread(L);
}

int __lua_resume(lua_State* L, int narg) {
	return lua_resume(L, NULL, narg);
}

int __lua_yield(lua_State* L, int nresults) {
	return lua_yield(L, nresults);
}

void __lua_xmove(lua_State* from, lua_State* to, int n) {
	lua_xmove(from, to, n);
}

// debug interface

int __lua_getstack(lua_State* L, int level, __lua_Debug* ar) {
	lua_Debug d;
	__zeroDbgIn(&d);
	__dbgCopyIn(ar, &d);
	int r = lua_getstack(L, level, &d);
	__dbgCopyOut(ar, &d);
	return r;
}

int __lua_getinfo(lua_State* L, const char* what, __lua_Debug* ar) {
	lua_Debug d;
	__zeroDbgIn(&d);
	__dbgCopyIn(ar, &d);
	int r = lua_getinfo(L, what, &d);
	__dbgCopyOut(ar, &d);
	return r;
}

const char* __lua_getlocal(lua_State* L, const __lua_Debug* ar, int n) {
	lua_Debug d;
	__zeroDbgIn(&d);
	__dbgCopyIn(ar, &d);
	return lua_getlocal(L, &d, n);
}

const char* __lua_setlocal(lua_State* L, const __lua_Debug* ar, int n) {
	lua_Debug d;
	__zeroDbgIn(&d);
	__dbgCopyIn(ar, &d);
	return lua_setlocal(L, &d, n);
}

const char* __lua_setupvalue(lua_State* L, int funcindex, int n) {
	if (__isregindex(funcindex)) {
		funcindex = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(funcindex)) {
		funcindex = __getupvalindex(funcindex);
	}
	return lua_setupvalue(L, funcindex, n);
}

const char* __lua_getupvalue(lua_State* L, int funcindex, int n) {
	if (__isregindex(funcindex)) {
		funcindex = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(funcindex)) {
		funcindex = __getupvalindex(funcindex);
	}
	return lua_getupvalue(L, funcindex, n);
}

void __lua_hook_replacement (lua_State* L, lua_Debug* ar) {
	__lua_adddata** a = lua_getextraspace(L);
	__lua_Debug r;
	__zeroDbg(&r);
	__dbgCopyOut(&r, ar);
	(*a)->hook(L, &r);
}

int __lua_sethook(lua_State* L, __lua_Hook func, int mask, int count) { // just good hooks are global
	__lua_adddata** a = lua_getextraspace(L);
	(*a)->hook = func;
	lua_sethook(L, &__lua_hook_replacement, mask, count);
	return 0; // no idea what it returns
}

__lua_Hook __lua_gethook(lua_State* L) {
	__lua_adddata** a = lua_getextraspace(L);
	return (*a)->hook;
}

int __lua_gethookmask(lua_State* L) {
	return lua_gethookmask(L);
}

int __lua_gethookcount(lua_State* L) {
	return lua_gethookcount(L);
}

// other funcs

const char* __lua_version(void) {
	return LUA_VERSION;
}

int __luaL_error(lua_State* L, const char* fmt, ...) { // has no va_list variant so i have to rewrite it
	va_list argp;
	va_start(argp, fmt);
	luaL_where(L, 1);
	lua_pushvfstring(L, fmt, argp);
	va_end(argp);
	lua_concat(L, 2);
	return lua_error(L);
}

int __lua_pushupvalues(lua_State* L) { // pushed all upvalues of c func at stack top
	lua_Debug d;
	lua_getinfo(L, ">u", &d); // get number upvalues
	int f = lua_gettop(L);
	if (!lua_checkstack(L, d.nups))
		return 0;
	for (int i = 0; i < d.nups; i++) {
		lua_getupvalue(L, f, i);
	}
	return d.nups;
}

int __luaL_findstring(const char* name, const char* const list[]) {
	int i;
	for (i = 0; list[i]; i++)
		if (strcmp(list[i], name) == 0)
			return i;
	return -1;  /* name not found */
}

int __luaL_getn(lua_State* L, int t) {
	if (__isregindex(t)) {
		t = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(t)) {
		t = __getupvalindex(t);
	}
	return (int) luaL_len(L, t);
}

void __luaL_setn(lua_State* L, int t, int n) { // table sizes are no longer a thing, so just make sure next stops there
	if (__isregindex(t)) {
		t = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(t)) {
		t = __getupvalindex(t);
	}
	else {
		t = lua_absindex(L, t);
	}
	lua_pushnil(L);
	lua_rawseti(L, t, n+1);
}

int __luaL_typerror(lua_State* L, int narg, const char* tname) {
	const char* msg = lua_pushfstring(L, "%s expected, got %s", tname, lua_typename(L, lua_type(L, narg)));
	return luaL_argerror(L, narg, msg);
}

int __luaL_argerror(lua_State* L, int narg, const char* extramsg) {
	return luaL_argerror(L, narg, extramsg);
}

void __luaL_where(lua_State* L, int level) {
	luaL_where(L, level);
}

// doXXX

int __lua_dofile(lua_State* L, const char* filename) {
	return luaL_dofile(L, filename);
}

int __lua_dobuffer(lua_State* L, const char* buff, size_t size, const char* name) {
	int s = luaL_loadbuffer(L, buff, size, name);
	if (s == 0) {
		s = lua_pcall(L, 0, LUA_MULTRET, 0);
	}
	return s;
}

int __lua_dostring(lua_State* L, const char* str) {
	return luaL_dostring(L, str);
}

int __lua_Writer(lua_State* L, const void* p, size_t sz, void* ud) {
	size_t writeBlockSz;
	struct __chunkwriterData* d = ud;
	do
	{
		writeBlockSz = (sz > 8000) ? 8000 : sz;
		d->writer(L, p, writeBlockSz, d->data); // lua 5.0 doesnt seem to have an option to end writing
		p = (void*)((int)p + writeBlockSz);
		sz -= writeBlockSz;
	}
	while (sz > 0);
	return 0;
}

int __lua_dump(lua_State* L, __lua_Chunkwriter writer, void* data) {
	if (!lua_isfunction(L, -1) || lua_iscfunction(L, -1))
		return 0;
	struct __chunkwriterData d;
	d.data = data;
	d.writer = writer;
	int r = lua_dump(L, &__lua_Writer, &d, 0);
	return !r;
}

//load

int __luaL_loadbuffer(lua_State* L, const char* buff, size_t size, const char* name) {
	return luaL_loadbuffer(L, buff, size, name);
}

int __luaL_loadfile(lua_State* L, const char* filename) {
	return luaL_loadfile(L, filename);
}

// buffer

void __luaL_buffinit(lua_State* L, __luaL_Buffer* B) {
	luaL_Buffer* nb = malloc(sizeof(luaL_Buffer));
	B->realbuff = nb;
	luaL_buffinit(L, nb);
}

void __luaL_addlstring(__luaL_Buffer* B, const char* s, size_t l) {
	luaL_addlstring(B->realbuff, s, l);
}

void __luaL_addstring(__luaL_Buffer* B, const char* s) {
	luaL_addstring(B->realbuff, s);
}

void __luaL_addvalue(__luaL_Buffer* B) {
	luaL_addvalue(B->realbuff);
}

void __luaL_pushresult(__luaL_Buffer* B) {
	luaL_pushresult(B->realbuff);
	free(B->realbuff);
}

char* __luaL_prepbuffer(__luaL_Buffer* B) {
	return luaL_prepbuffer(B->realbuff);
}

// metatable

int __luaL_callmeta(lua_State* L, int obj, const char* event) {
	if (__isregindex(obj)) {
		obj = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(obj)) {
		obj = __getupvalindex(obj);
	}
	return luaL_callmeta(L, obj, event);
}

int __luaL_getmetafield(lua_State* L, int obj, const char* event) {
	if (__isregindex(obj)) {
		obj = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(obj)) {
		obj = __getupvalindex(obj);
	}
	return luaL_getmetafield(L, obj, event);
}

void  __luaL_getmetatable(lua_State* L, const char* tname) {
	luaL_getmetatable(L, tname);
}

int __luaL_newmetatable(lua_State* L, const char* tname) {
	return luaL_newmetatable(L, tname);
}

// checkXXX

void __luaL_checkany(lua_State* L, int narg) {
	luaL_checkany(L, narg);
}

const char* __luaL_checklstring(lua_State* L, int narg, size_t* len) {
	if (__isregindex(narg)) {
		narg = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(narg)) {
		narg = __getupvalindex(narg);
	}
	return luaL_checklstring(L, narg, len);
}

lua_Number __luaL_checknumber(lua_State* L, int narg) {
	if (__isregindex(narg)) {
		narg = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(narg)) {
		narg = __getupvalindex(narg);
	}
	return luaL_checknumber(L, narg);
}

void __luaL_checkstack(lua_State* L, int space, const char* mes) {
	luaL_checkstack(L, space, mes);
}

void __luaL_checktype(lua_State* L, int narg, int t) {
	if (__isregindex(narg)) {
		narg = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(narg)) {
		narg = __getupvalindex(narg);
	}
	luaL_checktype(L, narg, t);
}

void* __luaL_checkudata(lua_State* L, int ud, const char* tname) {
	if (__isregindex(ud)) {
		ud = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(ud)) {
		ud = __getupvalindex(ud);
	}
	return luaL_checkudata(L, ud, tname);
}

const char* __luaL_optlstring(lua_State* L, int narg, const char* def, size_t* len) {
	if (__isregindex(narg)) {
		narg = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(narg)) {
		narg = __getupvalindex(narg);
	}
	return luaL_optlstring(L, narg, def, len);
}

lua_Number __luaL_optnumber(lua_State* L, int narg, lua_Number def) {
	if (__isregindex(narg)) {
		narg = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(narg)) {
		narg = __getupvalindex(narg);
	}
	return luaL_optnumber(L, narg, def);
}

// openlibs

void __luaL_openlib(lua_State* L, const char* libname, const luaL_Reg* l, int nup) {
	lua_newtable(L);
	luaL_setfuncs(L, l, nup);
	lua_setglobal(L, libname);
}

int __luaopen_base(lua_State* L) {
	return 1;
}

int __luaopen_debug(lua_State* L) {
	return 1;
}

int __luaopen_io(lua_State* L) {
	return 1;
}

int __luaopen_loadlib(lua_State* L) {
	return 1;
}

int __luaopen_math(lua_State* L) {
	return 1;
}

int __luaopen_string(lua_State* L) {
	return 1;
}

int __luaopen_table(lua_State* L) {
	return 1;
}

// ref

int __luaL_ref(lua_State* L, int t) {
	if (__isregindex(t)) {
		t = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(t)) {
		t = __getupvalindex(t);
	}
	return luaL_ref(L, t);
}

void __luaL_unref(lua_State* L, int t, int ref) {
	if (__isregindex(t)) {
		t = LUA_REGISTRYINDEX;
	}
	else if (__isupvalIndex(t)) {
		t = __getupvalindex(t);
	}
	luaL_unref(L, t, ref);
}


// bb funcs

int __lua_bb_getsdkversion() {
	return 1;
}

void* __lua_bb_getuserstate(lua_State* L) {
	__lua_adddata** a = lua_getextraspace(L);
	return &((*a)->bb_userstate);
}
