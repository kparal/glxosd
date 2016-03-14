/*
 * Copyright (C) 2013-2016 Nick Guletskii
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#define _GNU_SOURCE
#include "glinject.h"
#include "elfhacks.hpp"
#include <dlfcn.h>
#include <GL/gl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <lua.h>
#include <lualib.h>
#include <luajit.h>
#include <lauxlib.h>
#include <pthread.h>

pthread_mutex_t glinject_mutex = PTHREAD_MUTEX_INITIALIZER;

void init_gl_frame_hooks();
void handle_key_event(XKeyEvent* event);
void handle_configure_notify_event(XEvent* event);

static bool initialised = false;
static lua_State *L = 0;

typedef Bool (*XIfEvent_predicate_type)(Display* display, XEvent* event,
		XPointer pointer);
/*
 * Symbol definitions
 */

#define DEFINE_REAL_SYMBOL(name, ret, param)\
		typedef ret (*glinject_##name##_type) param;\
		glinject_##name##_type glinject_real_##name;

DEFINE_REAL_SYMBOL(dlopen, void*, (const char *, int));

DEFINE_REAL_SYMBOL(dlsym, void*, (const void *, const char *));

DEFINE_REAL_SYMBOL(dlvsym, void*, (const void *, const char *, const char *));

DEFINE_REAL_SYMBOL(glXGetProcAddressARB, __GLXextFuncPtr, (const GLubyte*));

DEFINE_REAL_SYMBOL(glXGetProcAddress, __GLXextFuncPtr, (const GLubyte*));

#define DEFINE_AND_OVERLOAD(name, ret, param)\
		DEFINE_REAL_SYMBOL(name, ret, param);\
		ret name param

#define LOAD_SYMBOL_USING_DLSYM(lib, x)\
	glinject_real_##x = (glinject_##x##_type) \
	glinject_real_dlsym(lib, #x);\
	assertSymbolLoaded(glinject_real_##x, #x);

#define LOAD_SYMBOL_USING_GLGETPROCADDR(x)\
	glinject_real_##x = (glinject_##x##_type) \
	glinject_real_glXGetProcAddress((const GLubyte*)#x);

void handle_event(XEvent* event) {
	if (event == NULL)
		return;

	if (event->type == KeyPress) {
		pthread_mutex_lock(&glinject_mutex);
		handle_key_event(event);
		pthread_mutex_unlock(&glinject_mutex);
	}

}
DEFINE_AND_OVERLOAD(glXDestroyPixmap, void, (Display *dpy, GLXPixmap pixmap)) {
	init_gl_frame_hooks();
	handle_context_destruction(dpy, pixmap);
	glinject_real_glXDestroyPixmap(dpy, pixmap);
}

DEFINE_AND_OVERLOAD( glXDestroyWindow,void,(Display *dpy, GLXWindow win) ) {
	init_gl_frame_hooks();
	handle_context_destruction(dpy, win);
	glinject_real_glXDestroyWindow(dpy, win);
}

DEFINE_AND_OVERLOAD( glXDestroyGLXPixmap,void,(Display *dpy, GLXPixmap pix)) {
	init_gl_frame_hooks();
	handle_context_destruction(dpy, pix);
	glinject_real_glXDestroyGLXPixmap(dpy, pix);
}

DEFINE_AND_OVERLOAD( glXDestroyPbuffer,void,(Display *dpy, GLXPbuffer pbuf)) {
	init_gl_frame_hooks();
	handle_context_destruction(dpy, pbuf);
	glinject_real_glXDestroyPbuffer(dpy, pbuf);
}

DEFINE_AND_OVERLOAD( glXDestroyContext,void,(Display *dpy, GLXContext ctx) ) {
	init_gl_frame_hooks();
	handle_context_destruction(dpy, ctx);
	glinject_real_glXDestroyContext(dpy, ctx);
}
DEFINE_AND_OVERLOAD( glXSwapBuffers, void,(Display* dpy, GLXDrawable ctx) ) {
	init_gl_frame_hooks();
	handle_buffer_swap(dpy, ctx);
	glinject_real_glXSwapBuffers(dpy, ctx);
}

DEFINE_AND_OVERLOAD
(XCheckIfEvent, Bool, (Display* display, XEvent* event, XIfEvent_predicate_type predicate, XPointer arg)) {
	Bool return_val = glinject_real_XCheckIfEvent(display, event, predicate,
			arg);
	if (return_val) {
		handle_event(event);
	}
	return return_val;
}
DEFINE_AND_OVERLOAD(XIfEvent, int,
		(Display* display, XEvent* event, XIfEvent_predicate_type predicate, XPointer pointer)) {
	int return_val = glinject_real_XIfEvent(display, event, predicate, pointer);
	handle_event(event);
	return return_val;
}

DEFINE_AND_OVERLOAD(XMaskEvent, int,
		(Display* display, long mask, XEvent* event)) {
	int return_val = glinject_real_XMaskEvent(display, mask, event);
	handle_event(event);
	return return_val;
}

DEFINE_AND_OVERLOAD(XNextEvent, int,
		(Display* display, XEvent* event)) {
	int return_val = glinject_real_XNextEvent(display, event);
	handle_event(event);
	return return_val;
}

DEFINE_AND_OVERLOAD(XWindowEvent, int,
		(Display* display, Window window, long mask, XEvent* event)) {
	int return_val = glinject_real_XWindowEvent(display, window, mask, event);
	handle_event(event);
	return return_val;
}

/*
 * Initialisation of the overriden functions list
 */

void* get_function_override(const char* name) {
	if (strcmp("glXSwapBuffers", name) == 0) {
		return &glXSwapBuffers;
	}
	if (strcmp("glXDestroyContext", name) == 0) {
		return &glXDestroyContext;
	}
	if (strcmp("glXGetProcAddressARB", name) == 0) {
		return &glXGetProcAddressARB;
	}
	if (strcmp("glXGetProcAddress", name) == 0) {
		return &glXGetProcAddress;
	}
	if (strcmp("dlsym", name) == 0) {
		return &dlsym;
	}
	if (strcmp("dlvsym", name) == 0) {
		return &dlvsym;
	}
	return NULL ;
}

void pushx11keymodifiers(lua_State* L, XKeyEvent *event) {
	lua_createtable(L, 0, 4);
	lua_pushboolean(L, (event->state & ShiftMask) != 0);
	lua_setfield(L, -2, "shift");
	lua_pushboolean(L, (event->state & LockMask) != 0);
	lua_setfield(L, -2, "caps");
	lua_pushboolean(L, (event->state & ControlMask) != 0);
	lua_setfield(L, -2, "control");
	lua_pushboolean(L, (event->state & Mod1Mask) != 0);
	lua_setfield(L, -2, "alt");
}

/*
 * Handlers
 */
Bool check_if_event(XEvent* event) {
	pthread_mutex_lock(&glinject_mutex);
	if (event->type == ConfigureNotify) {
		lua_getglobal(L, "should_consume_configure_notify_event"); /* function to be called */
		if (!lua_isfunction(L, -1)) {
			fprintf(stderr,
					"should_consume_configure_notify_event is not a function!\n");
			return False;
		}
		pushx11keymodifiers(L, event);
		if (lua_pcall(L, 1, 1, 0) != 0) {
			fprintf(stderr, "error running function: %s\n",
					lua_tostring(L, -1));
		}
		if (!lua_isboolean(L, -1)) {
			fprintf(stderr,
					"should_consume_configure_notify_event must return a boolean\n");
		}
		Bool res = lua_toboolean(L, -1);
		lua_pop(L, 1);
		return res;
	}
	if (event->type == KeyPress) {
		lua_getglobal(L, "should_consume_key_press_event"); /* function to be called */
		if (!lua_isfunction(L, -1)) {
			fprintf(stderr,
					"should_consume_key_press_event is not a function!\n");
			return False;
		}

		KeySym ks = XLookupKeysym(event, 0);

		lua_pushstring(L, XKeysymToString(ks));
		pushx11keymodifiers(L, event);
		if (lua_pcall(L, 2, 1, 0) != 0) {
			fprintf(stderr, "error running function: %s\n",
					lua_tostring(L, -1));
		}
		if (!lua_isboolean(L, -1)) {
			fprintf(stderr,
					"should_consume_key_press_event must return a boolean\n");
		}
		Bool res = lua_toboolean(L, -1);
		lua_pop(L, 1);
		return res;
	}
	pthread_mutex_unlock(&glinject_mutex);
	return False;
}

void handle_buffer_swap(Display* dpy, GLXDrawable ctx) {
	XEvent event;
	XCheckIfEvent(dpy, &event, check_if_event, NULL);

	pthread_mutex_lock(&glinject_mutex);
	lua_getglobal(L, "handle_buffer_swap"); /* function to be called */
	if (!lua_isfunction(L, -1)) {
		fprintf(stderr, "handle_buffer_swap is not a function!\n");
		return;
	}
	lua_pushlightuserdata(L, dpy);
	lua_pushnumber(L, ctx);
	if (lua_pcall(L, 2, 0, 0) != 0) {
		fprintf(stderr, "error running function: %s\n", lua_tostring(L, -1));
	}
	pthread_mutex_unlock(&glinject_mutex);
}

void handle_context_destruction(Display* dpy, GLXDrawable ctx) {
	pthread_mutex_lock(&glinject_mutex);
	lua_getglobal(L, "handle_context_destruction"); /* function to be called */
	if (!lua_isfunction(L, -1)) {
		fprintf(stderr, "handle_context_destruction is not a function!\n");
		return;
	}
	lua_pushlightuserdata(L, dpy);
	lua_pushnumber(L, ctx);
	if (lua_pcall(L, 2, 0, 0) != 0) {
		fprintf(stderr, "error running function: %s\n", lua_tostring(L, -1));
	}
	pthread_mutex_unlock(&glinject_mutex);
}

void handle_key_event(XKeyEvent* event) {
	KeySym ks = XLookupKeysym(event, 0);

	lua_getglobal(L, "key_press_event"); /* function to be called */
	if (!lua_isfunction(L, -1)) {
		fprintf(stderr, "key_press is not a function!\n");
		return;
	}
	lua_pushstring(L, XKeysymToString(ks));
	pushx11keymodifiers(L, event);

	if (lua_pcall(L, 2, 0, 0) != 0) {
		fprintf(stderr, "error running function: %s\n", lua_tostring(L, -1));
	}
}
void handle_configure_notify_event(XEvent* event) {
	lua_getglobal(L, "configure_notify_event"); /* function to be called */
	if (!lua_isfunction(L, -1)) {
		fprintf(stderr, "configure_notify_event is not a function!\n");
		return;
	}
	// lua_pushlightuserdata(L, event);
	if (lua_pcall(L, 0, 0, 0) != 0) {
		fprintf(stderr, "error running function: %s\n", lua_tostring(L, -1));
	}
}

static void assertSymbolLoaded(void* symbol, const char* name) {
	if (symbol == NULL) {
		fprintf(stderr, "Couldn't find %s! dlsym returned a NULL pointer: %s\n",
				name, dlerror());
		exit(-1);
	}
}
/*
 * Function overrides
 */
__GLXextFuncPtr glXGetProcAddressARB(const GLubyte *name) {
	init_gl_frame_hooks();
	void* overriddenFunction = get_function_override((const char *) name);
	if (overriddenFunction != NULL)
		return (__GLXextFuncPtr) overriddenFunction;
	return glinject_real_glXGetProcAddressARB(name);
}
__GLXextFuncPtr glXGetProcAddress(const GLubyte *name) {
	init_gl_frame_hooks();
	void* overriddenFunction = get_function_override((const char *) name);
	if (overriddenFunction != NULL)
		return (__GLXextFuncPtr) overriddenFunction;
	return glinject_real_glXGetProcAddress(name);
}

void * dlsym(void *handle, const char *name) {
	init_gl_frame_hooks();
	void* overload = get_function_override(name);
	if (overload != NULL)
		return overload;
	return glinject_real_dlsym(handle, name);
}

void *dlvsym(void *handle, const char *name, const char *ver) {
	init_gl_frame_hooks();
	void* overload = get_function_override(name);
	if (overload != NULL)
		return overload;
	return glinject_real_dlvsym(handle, name, ver);
}

gl_function_provider_type get_gl_function_provider() {
	init_gl_frame_hooks();
	return glinject_real_glXGetProcAddress;
}

void init_gl_frame_hooks() {
	if (initialised)
		return;
	initialised = true;

	/*
	 * Load dlsym and dlvsym using libelfhacks because if we were to use dlsym
	 * to load dlsym, we would get our overridden dlsym, which would result in
	 * recursion.
	 * */

	eh_obj_t libdl;
	if (eh_find_obj(&libdl, "*/libdl.so*")) {
		fprintf(stderr, "Couldn't find libdl!\n");
		exit(-1);
	}
	if (eh_find_sym(&libdl, "dlsym", (void **) &glinject_real_dlsym)) {
		fprintf(stderr, "Couldn't find dlsym in libdl!\n");
		eh_destroy_obj(&libdl);
		exit(-1);
	}
	if (eh_find_sym(&libdl, "dlvsym", (void **) &glinject_real_dlvsym)) {
		fprintf(stderr, "Couldn't find dlvsym in libdl!\n");
		eh_destroy_obj(&libdl);
		exit(-1);
	}
	eh_destroy_obj(&libdl);

	glinject_real_dlopen = (glinject_dlopen_type) glinject_real_dlsym(RTLD_NEXT,
			"dlopen");
	if (glinject_real_dlopen == NULL) {
		fprintf(stderr,
				"Couldn't find dlopen! dlsym returned a NULL pointer. %s\n",
				dlerror());
		exit(-1);
	}

	void *handle = dlopen("libGL.so.1", RTLD_LOCAL | RTLD_LAZY);
	void *handle2 = dlopen("libX11.so.6", RTLD_LOCAL | RTLD_LAZY);
	LOAD_SYMBOL_USING_DLSYM(handle, glXDestroyContext);
	LOAD_SYMBOL_USING_DLSYM(handle, glXGetProcAddressARB);
	LOAD_SYMBOL_USING_DLSYM(handle, glXGetProcAddress);
	LOAD_SYMBOL_USING_DLSYM(handle, glXSwapBuffers);
	LOAD_SYMBOL_USING_DLSYM(handle2, XIfEvent);
	LOAD_SYMBOL_USING_DLSYM(handle2, XCheckIfEvent);
	LOAD_SYMBOL_USING_DLSYM(handle2, XMaskEvent);
	LOAD_SYMBOL_USING_DLSYM(handle2, XNextEvent);
	LOAD_SYMBOL_USING_DLSYM(handle2, XWindowEvent);
}
void glinject_construct() {
	L = luaL_newstate();
	if (!L) {
		fprintf(stderr, "Lua initialization failed.");
		exit(-1);
	}
	luaL_openlibs(L);

	lua_pushliteral(L, LUA_SOURCECODE_PATH);
	lua_setfield(L, LUA_GLOBALSINDEX, "glxosdPackageRoot");

	const char* path = getenv("GLINJECT_BOOTSTRAP_SCRIPT");
	if (path == NULL || strlen(path) == 0) {
		fprintf(stderr, "No glinject bootstrap script specified!");
		exit(1);
	}

	int status = luaL_loadfile(L, path);

	if (status) {
		fprintf(stderr, "Couldn't load file: %s\n", lua_tostring(L, -1));
		exit(1);
	}
	int ret = lua_pcall(L, 0, 0, 0);
	if (ret != 0) {
		fprintf(stderr, "%s\n", lua_tostring(L, -1));
		exit(-1);
	}
}
void glinject_destruct() {
	lua_close(L);
}
