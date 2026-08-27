/* anglecheck: replay WebKitGTK's WebGL context creation outside the browser.
 *
 * WebKit's WebGL runs on its bundled ANGLE, which sits on top of Mesa's EGL.
 * A page that says "WebGL is disabled or unavailable" got a null context and
 * WebKit's release build logs nothing about why.  This program performs the
 * same calls WebKit makes -- PlatformDisplay::angleEGLDisplay(),
 * angleSharingGLContext() and
 * GraphicsContextGLTextureMapperANGLE::platformInitializeContext() -- against
 * the very ANGLE objects linked into libwebkit2gtk, and reports the first one
 * that fails together with EGL_GetError().  Built by the top-level Makefile
 * against the WebKit build tree's libGLESv2.a/libANGLE.a (thin archives, so
 * the tree must be present). */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

typedef void *EGLDisplay; typedef void *EGLConfig; typedef void *EGLContext; typedef void *EGLSurface;
typedef int EGLint; typedef unsigned EGLBoolean; typedef unsigned EGLenum;
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_DEFAULT_DISPLAY ((void*)0)
#define EGL_FALSE 0
#define EGL_TRUE 1
#define EGL_NONE 0x3038
#define EGL_SUCCESS 0x3000
#define EGL_EXTENSIONS 0x3055
#define EGL_VENDOR 0x3053
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_DEPTH_SIZE 0x3025
#define EGL_STENCIL_SIZE 0x3026
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#define EGL_PLATFORM_ANGLE_TYPE_OPENGLES_ANGLE 0x320E
#define EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE 0x3209
#define EGL_PLATFORM_ANGLE_DEVICE_TYPE_EGL_ANGLE 0x348E
#define EGL_PLATFORM_ANGLE_NATIVE_PLATFORM_TYPE_ANGLE 0x348F
#define EGL_EXTERNAL_CONTEXT_ANGLE 0x348E
#define EGL_CONTEXT_OPENGL_BACKWARDS_COMPATIBLE_ANGLE 0x3483
#define EGL_CONTEXT_WEBGL_COMPATIBILITY_ANGLE 0x33AC
#define EGL_ROBUST_RESOURCE_INITIALIZATION_ANGLE 0x3453
#define EGL_CONTEXT_CLIENT_ARRAYS_ENABLED_ANGLE 0x3452
#define EGL_CONTEXT_BIND_GENERATES_RESOURCE_CHROMIUM 0x33AD
#define EGL_CONTEXT_VIRTUALIZATION_GROUP_ANGLE 0x3481
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02

/* ANGLE's entry points, as WebKit builds them (EGL_EGL_PROTOTYPES=0, EGL_ prefix). */
extern "C" {
EGLDisplay EGL_GetPlatformDisplayEXT(EGLenum, void *, const EGLint *);
EGLBoolean EGL_Initialize(EGLDisplay, EGLint *, EGLint *);
EGLint EGL_GetError(void);
const char *EGL_QueryString(EGLDisplay, EGLint);
EGLBoolean EGL_ChooseConfig(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
EGLContext EGL_CreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
EGLBoolean EGL_MakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
EGLBoolean EGL_BindAPI(EGLenum);
const unsigned char *GL_GetString(unsigned);
}

static void step(const char *what, bool ok) {
    EGLint e = EGL_GetError();
    printf("%-52s %s  (EGL_GetError=0x%x)\n", what, ok ? "ok" : "FAILED", e);
    fflush(stdout);
}

int main(int argc, char **argv) {
    bool webgl2 = argc > 1 && !strcmp(argv[1], "2");

    /* 1. The web process's own Mesa context, which WebKit makes current
     *    before asking ANGLE for the "external" sharing context. */
    void *mesa = dlopen("libEGL.so.1", RTLD_NOW);
    if (!mesa) { printf("dlopen libEGL.so.1 FAILED: %s\n", dlerror()); return 1; }
    typedef void *(*gpa_t)(const char *);
    gpa_t gpa = (gpa_t)dlsym(mesa, "eglGetProcAddress");
    EGLDisplay (*m_getPlatformDisplayEXT)(EGLenum, void *, const EGLint *) = (EGLDisplay(*)(EGLenum, void *, const EGLint *))gpa("eglGetPlatformDisplayEXT");
    EGLBoolean (*m_initialize)(EGLDisplay, EGLint *, EGLint *) = (EGLBoolean(*)(EGLDisplay, EGLint *, EGLint *))gpa("eglInitialize");
    EGLBoolean (*m_chooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *) = (EGLBoolean(*)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *))gpa("eglChooseConfig");
    EGLContext (*m_createContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *) = (EGLContext(*)(EGLDisplay, EGLConfig, EGLContext, const EGLint *))gpa("eglCreateContext");
    EGLBoolean (*m_makeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = (EGLBoolean(*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext))gpa("eglMakeCurrent");
    EGLBoolean (*m_bindAPI)(EGLenum) = (EGLBoolean(*)(EGLenum))gpa("eglBindAPI");
    EGLint (*m_getError)(void) = (EGLint(*)(void))gpa("eglGetError");

    EGLDisplay mdpy = m_getPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, 0);
    printf("mesa surfaceless display: %p (err 0x%x)\n", mdpy, m_getError());
    EGLint maj = 0, min = 0;
    printf("mesa eglInitialize: %u -> %d.%d (err 0x%x)\n", m_initialize(mdpy, &maj, &min), maj, min, m_getError());
    m_bindAPI(EGL_OPENGL_ES_API);
    const EGLint mcfg[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLConfig mconfig = 0; EGLint n = 0;
    printf("mesa eglChooseConfig: %u n=%d (err 0x%x)\n", m_chooseConfig(mdpy, mcfg, &mconfig, 1, &n), n, m_getError());
    const EGLint mctx[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext mesaCtx = m_createContext(mdpy, mconfig, EGL_NO_CONTEXT, mctx);
    printf("mesa eglCreateContext: %p (err 0x%x)\n", mesaCtx, m_getError());
    printf("mesa eglMakeCurrent(surfaceless): %u (err 0x%x)\n", m_makeCurrent(mdpy, EGL_NO_SURFACE, EGL_NO_SURFACE, mesaCtx), m_getError());

    /* 2. PlatformDisplay::angleEGLDisplay() */
    const EGLint dattr[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_OPENGLES_ANGLE,
        EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_DEVICE_TYPE_EGL_ANGLE,
        EGL_PLATFORM_ANGLE_NATIVE_PLATFORM_TYPE_ANGLE, EGL_PLATFORM_SURFACELESS_MESA,
        EGL_NONE };
    EGLDisplay dpy = EGL_GetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, dattr);
    step("ANGLE EGL_GetPlatformDisplayEXT", dpy != EGL_NO_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) return 2;
    maj = min = 0;
    EGLBoolean ok = EGL_Initialize(dpy, &maj, &min);
    step("ANGLE EGL_Initialize", ok);
    if (!ok) return 3;
    printf("  ANGLE EGL %d.%d, vendor: %s\n", maj, min, EGL_QueryString(dpy, EGL_VENDOR));
    const char *ext = EGL_QueryString(dpy, EGL_EXTENSIONS);
    printf("  ANGLE display extensions: %s\n", ext ? ext : "(null)");

    /* 3. PlatformDisplay::angleSharingGLContext() */
    const EGLint cfgAttr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_NONE };
    EGLConfig config = 0; n = 0;
    ok = EGL_ChooseConfig(dpy, cfgAttr, &config, 1, &n);
    step("ANGLE EGL_ChooseConfig (pbuffer RGBA8)", ok && n == 1);
    printf("  configs returned: %d\n", n);
    if (!ok || n != 1) return 4;
    const EGLint shareAttr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_EXTERNAL_CONTEXT_ANGLE, EGL_TRUE, EGL_NONE };
    EGLContext sharing = EGL_CreateContext(dpy, config, EGL_NO_CONTEXT, shareAttr);
    step("ANGLE sharing context (EGL_EXTERNAL_CONTEXT_ANGLE)", sharing != EGL_NO_CONTEXT);
    if (sharing == EGL_NO_CONTEXT) return 5;

    /* 4. GraphicsContextGLTextureMapperANGLE::platformInitializeContext() */
    EGL_BindAPI(EGL_OPENGL_ES_API);
    step("ANGLE EGL_BindAPI(ES)", EGL_GetError() == EGL_SUCCESS);
    EGLint ctxAttr[24]; int i = 0;
    ctxAttr[i++] = EGL_CONTEXT_CLIENT_VERSION; ctxAttr[i++] = webgl2 ? 3 : 2;
    if (!webgl2) { ctxAttr[i++] = EGL_CONTEXT_OPENGL_BACKWARDS_COMPATIBLE_ANGLE; ctxAttr[i++] = EGL_FALSE; }
    ctxAttr[i++] = EGL_CONTEXT_WEBGL_COMPATIBILITY_ANGLE; ctxAttr[i++] = EGL_TRUE;
    ctxAttr[i++] = EGL_ROBUST_RESOURCE_INITIALIZATION_ANGLE; ctxAttr[i++] = EGL_TRUE;
    ctxAttr[i++] = EGL_CONTEXT_CLIENT_ARRAYS_ENABLED_ANGLE; ctxAttr[i++] = EGL_FALSE;
    ctxAttr[i++] = EGL_CONTEXT_BIND_GENERATES_RESOURCE_CHROMIUM; ctxAttr[i++] = EGL_FALSE;
    ctxAttr[i++] = EGL_CONTEXT_VIRTUALIZATION_GROUP_ANGLE; ctxAttr[i++] = 0;
    ctxAttr[i++] = EGL_NONE;
    EGLContext ctx = EGL_CreateContext(dpy, config, sharing, ctxAttr);
    step(webgl2 ? "ANGLE WebGL2 context" : "ANGLE WebGL1 context", ctx != EGL_NO_CONTEXT);
    if (ctx == EGL_NO_CONTEXT) return 6;
    ok = EGL_MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
    step("ANGLE EGL_MakeCurrent", ok);
    if (!ok) return 7;
    printf("  GL_RENDERER: %s\n  GL_VERSION:  %s\n", GL_GetString(GL_RENDERER), GL_GetString(GL_VERSION));
    printf("ALL STEPS PASSED\n");
    return 0;
}
