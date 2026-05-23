/*
 * Minimal GLX ABI stubs for TAWC's GLES-only libhybris backend.
 *
 * These functions intentionally do not implement desktop GLX. They only make
 * GLX probes and hard DT_NEEDED references fail softly so clients can fall
 * back to EGL/GLES instead of loading Mesa's GLX stack inside the rootfs.
 */

#include <stddef.h>

typedef int Bool;
typedef unsigned int GLuint;
typedef unsigned long GLXDrawable;
typedef unsigned long GLXPixmap;
typedef unsigned long GLXWindow;
typedef unsigned long GLXPbuffer;
typedef void (*__GLXextFuncPtr)(void);

typedef struct _XDisplay Display;
typedef struct XVisualInfo XVisualInfo;
typedef struct __GLXcontextRec *GLXContext;
typedef struct __GLXFBConfigRec *GLXFBConfig;

static void set_int(int *slot, int value)
{
    if (slot != NULL) {
        *slot = value;
    }
}

static void set_uint(unsigned int *slot, unsigned int value)
{
    if (slot != NULL) {
        *slot = value;
    }
}

GLXContext glXGetCurrentContext(void)
{
    return NULL;
}

Display *glXGetCurrentDisplay(void)
{
    return NULL;
}

GLXDrawable glXGetCurrentDrawable(void)
{
    return 0;
}

GLXDrawable glXGetCurrentReadDrawable(void)
{
    return 0;
}

__GLXextFuncPtr glXGetProcAddress(const unsigned char *name)
{
    (void)name;
    return NULL;
}

__GLXextFuncPtr glXGetProcAddressARB(const unsigned char *name)
{
    (void)name;
    return NULL;
}

Bool glXQueryExtension(Display *dpy, int *error_base, int *event_base)
{
    (void)dpy;
    set_int(error_base, 0);
    set_int(event_base, 0);
    return 0;
}

Bool glXQueryVersion(Display *dpy, int *major, int *minor)
{
    (void)dpy;
    set_int(major, 0);
    set_int(minor, 0);
    return 0;
}

const char *glXQueryExtensionsString(Display *dpy, int screen)
{
    (void)dpy;
    (void)screen;
    return "";
}

const char *glXGetClientString(Display *dpy, int name)
{
    (void)dpy;
    (void)name;
    return "";
}

const char *glXQueryServerString(Display *dpy, int screen, int name)
{
    (void)dpy;
    (void)screen;
    (void)name;
    return "";
}

XVisualInfo *glXChooseVisual(Display *dpy, int screen, int *attrib_list)
{
    (void)dpy;
    (void)screen;
    (void)attrib_list;
    return NULL;
}

GLXContext glXCreateContext(
    Display *dpy,
    XVisualInfo *vis,
    GLXContext share_list,
    Bool direct)
{
    (void)dpy;
    (void)vis;
    (void)share_list;
    (void)direct;
    return NULL;
}

void glXDestroyContext(Display *dpy, GLXContext ctx)
{
    (void)dpy;
    (void)ctx;
}

Bool glXMakeCurrent(Display *dpy, GLXDrawable drawable, GLXContext ctx)
{
    (void)dpy;
    (void)drawable;
    (void)ctx;
    return 0;
}

void glXSwapBuffers(Display *dpy, GLXDrawable drawable)
{
    (void)dpy;
    (void)drawable;
}

Bool glXIsDirect(Display *dpy, GLXContext ctx)
{
    (void)dpy;
    (void)ctx;
    return 0;
}

int glXGetConfig(Display *dpy, XVisualInfo *vis, int attrib, int *value)
{
    (void)dpy;
    (void)vis;
    (void)attrib;
    set_int(value, 0);
    return 1;
}

void glXCopyContext(Display *dpy, GLXContext src, GLXContext dst, unsigned long mask)
{
    (void)dpy;
    (void)src;
    (void)dst;
    (void)mask;
}

void glXWaitGL(void)
{
}

void glXWaitX(void)
{
}

void glXUseXFont(unsigned long font, int first, int count, int list_base)
{
    (void)font;
    (void)first;
    (void)count;
    (void)list_base;
}

GLXFBConfig *glXChooseFBConfig(
    Display *dpy,
    int screen,
    const int *attrib_list,
    int *nelements)
{
    (void)dpy;
    (void)screen;
    (void)attrib_list;
    set_int(nelements, 0);
    return NULL;
}

GLXFBConfig *glXGetFBConfigs(Display *dpy, int screen, int *nelements)
{
    (void)dpy;
    (void)screen;
    set_int(nelements, 0);
    return NULL;
}

int glXGetFBConfigAttrib(Display *dpy, GLXFBConfig config, int attribute, int *value)
{
    (void)dpy;
    (void)config;
    (void)attribute;
    set_int(value, 0);
    return 1;
}

XVisualInfo *glXGetVisualFromFBConfig(Display *dpy, GLXFBConfig config)
{
    (void)dpy;
    (void)config;
    return NULL;
}

GLXContext glXCreateNewContext(
    Display *dpy,
    GLXFBConfig config,
    int render_type,
    GLXContext share_list,
    Bool direct)
{
    (void)dpy;
    (void)config;
    (void)render_type;
    (void)share_list;
    (void)direct;
    return NULL;
}

Bool glXMakeContextCurrent(
    Display *dpy,
    GLXDrawable draw,
    GLXDrawable read,
    GLXContext ctx)
{
    (void)dpy;
    (void)draw;
    (void)read;
    (void)ctx;
    return 0;
}

GLXWindow glXCreateWindow(
    Display *dpy,
    GLXFBConfig config,
    unsigned long win,
    const int *attrib_list)
{
    (void)dpy;
    (void)config;
    (void)win;
    (void)attrib_list;
    return 0;
}

void glXDestroyWindow(Display *dpy, GLXWindow win)
{
    (void)dpy;
    (void)win;
}

GLXPixmap glXCreatePixmap(
    Display *dpy,
    GLXFBConfig config,
    unsigned long pixmap,
    const int *attrib_list)
{
    (void)dpy;
    (void)config;
    (void)pixmap;
    (void)attrib_list;
    return 0;
}

void glXDestroyPixmap(Display *dpy, GLXPixmap pixmap)
{
    (void)dpy;
    (void)pixmap;
}

GLXPbuffer glXCreatePbuffer(Display *dpy, GLXFBConfig config, const int *attrib_list)
{
    (void)dpy;
    (void)config;
    (void)attrib_list;
    return 0;
}

void glXDestroyPbuffer(Display *dpy, GLXPbuffer pbuf)
{
    (void)dpy;
    (void)pbuf;
}

void glXQueryDrawable(Display *dpy, GLXDrawable draw, int attribute, unsigned int *value)
{
    (void)dpy;
    (void)draw;
    (void)attribute;
    set_uint(value, 0);
}

int glXQueryContext(Display *dpy, GLXContext ctx, int attribute, int *value)
{
    (void)dpy;
    (void)ctx;
    (void)attribute;
    set_int(value, 0);
    return 1;
}

void glXSelectEvent(Display *dpy, GLXDrawable draw, unsigned long event_mask)
{
    (void)dpy;
    (void)draw;
    (void)event_mask;
}

void glXGetSelectedEvent(Display *dpy, GLXDrawable draw, unsigned long *event_mask)
{
    (void)dpy;
    (void)draw;
    if (event_mask != NULL) {
        *event_mask = 0;
    }
}

GLXContext glXCreateContextAttribsARB(
    Display *dpy,
    GLXFBConfig config,
    GLXContext share_context,
    Bool direct,
    const int *attrib_list)
{
    (void)dpy;
    (void)config;
    (void)share_context;
    (void)direct;
    (void)attrib_list;
    return NULL;
}

int glXSwapIntervalSGI(int interval)
{
    (void)interval;
    return 0;
}

int glXSwapIntervalMESA(unsigned int interval)
{
    (void)interval;
    return 0;
}

int glXGetSwapIntervalMESA(void)
{
    return 0;
}

void glXSwapIntervalEXT(Display *dpy, GLXDrawable drawable, int interval)
{
    (void)dpy;
    (void)drawable;
    (void)interval;
}
