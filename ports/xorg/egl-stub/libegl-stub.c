/*
 * libEGL.so.1 -- a stub that fails, on purpose.
 *
 * There is no GL or EGL implementation on this system: no Mesa, no libEGL, no
 * DRM device (ports/xorg/packages.list says why).  That is fine for rendering,
 * which goes through cairo -- but not for libepoxy, which resolves EGL entry
 * points by dlopen'ing libEGL.so.1 the moment one is CALLED and then does:
 *
 *     fprintf(stderr, "Couldn't open %s: %s", lib_name, dlerror());
 *     abort();
 *
 * So a caller that merely PROBES for EGL -- GTK asking whether it can make a
 * GL context, WebKit asking which buffer transports exist -- killed the
 * process outright instead of being told "no".  luakit's web process died and
 * respawned in a loop and pages never rendered; Claws Mail died opening an
 * HTML message.
 *
 * This library exists so that question can be answered.  Every entry point
 * returns zero, which is the failure value for every EGL return type that
 * matters: EGL_NO_DISPLAY, EGL_NO_CONTEXT, EGL_NO_SURFACE and EGL_FALSE are
 * all 0, and a NULL pointer is the failure return for the query calls.  A
 * caller that checks its result -- which callers must, since EGL can fail on
 * real hardware too -- takes its no-EGL path.
 *
 * It is NOT a step towards implementing EGL.  If something turns out to need
 * EGL to actually work rather than merely to answer, the answer is Mesa's
 * software rasteriser, not filling these in.  See tmp/WEBKIT-EGL-BLOCKER.md.
 *
 * Generated from the EGL registry that libepoxy ships
 * (gtk3/libepoxy-1.5.10/registry/egl.xml), so the symbol set matches exactly
 * what epoxy may dlsym.  A symbol epoxy asks for and does not find is fatal in
 * the same way a missing library is, which is why this exports all of them
 * rather than the handful a particular caller happens to use.
 *
 * The uniform no-argument signature is deliberate: these never read their
 * arguments, and on x86-64 a function that ignores its arguments and returns 0
 * in %rax is call-compatible with any of the real prototypes.
 */

void *__likeos_egl_stub(void);
void *__likeos_egl_stub(void) { return 0; }

void *eglBindAPI(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglBindTexImage(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglChooseConfig(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglClientSignalSyncEXT(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglClientWaitSync(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglClientWaitSyncKHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglClientWaitSyncNV(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglCompositorBindTexWindowEXT(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglCompositorSetContextAttributesEXT(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglCompositorSetContextListEXT(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglCompositorSetSizeEXT(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglCompositorSetWindowAttributesEXT(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglCompositorSetWindowListEXT(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglCompositorSwapPolicyEXT(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglCopyBuffers(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglCreateContext(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglCreateDRMImageMESA(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglCreateFenceSyncNV(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglCreateImage(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglCreateImageKHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglCreateNativeClientBufferANDROID(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglCreatePbufferFromClientBuffer(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglCreatePbufferSurface(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglCreatePixmapSurface(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglCreatePixmapSurfaceHI(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglCreatePlatformPixmapSurface(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglCreatePlatformPixmapSurfaceEXT(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglCreatePlatformWindowSurface(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglCreatePlatformWindowSurfaceEXT(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglCreateStreamAttribKHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglCreateStreamFromFileDescriptorKHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglCreateStreamKHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglCreateStreamProducerSurfaceKHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglCreateStreamSyncNV(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglCreateSync(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglCreateSync64KHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglCreateSyncKHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglCreateWindowSurface(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglDebugMessageControlKHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglDestroyContext(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglDestroyImage(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglDestroyImageKHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglDestroyStreamKHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglDestroySurface(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglDestroySync(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglDestroySyncKHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglDestroySyncNV(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglDupNativeFenceFDANDROID(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglExportDMABUFImageMESA(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglExportDMABUFImageQueryMESA(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglExportDRMImageMESA(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglFenceNV(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglGetCompositorTimingANDROID(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglGetCompositorTimingSupportedANDROID(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglGetConfigAttrib(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglGetConfigs(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglGetCurrentContext(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglGetCurrentDisplay(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglGetCurrentSurface(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglGetDisplay(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglGetDisplayDriverConfig(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglGetDisplayDriverName(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglGetError(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglGetFrameTimestampSupportedANDROID(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglGetFrameTimestampsANDROID(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglGetNativeClientBufferANDROID(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglGetNextFrameIdANDROID(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglGetOutputLayersEXT(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglGetOutputPortsEXT(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglGetPlatformDisplay(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglGetPlatformDisplayEXT(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglGetProcAddress(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglGetStreamFileDescriptorKHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglGetSyncAttrib(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglGetSyncAttribKHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglGetSyncAttribNV(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglGetSystemTimeFrequencyNV(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglGetSystemTimeNV(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglInitialize(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglLabelObjectKHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglLockSurfaceKHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglMakeCurrent(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglOutputLayerAttribEXT(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglOutputPortAttribEXT(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglPostSubBufferNV(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglPresentationTimeANDROID(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglQueryAPI(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglQueryContext(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglQueryDebugKHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglQueryDeviceAttribEXT(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglQueryDeviceStringEXT(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglQueryDevicesEXT(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglQueryDisplayAttribEXT(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglQueryDisplayAttribKHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglQueryDisplayAttribNV(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglQueryDmaBufFormatsEXT(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglQueryDmaBufModifiersEXT(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglQueryNativeDisplayNV(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglQueryNativePixmapNV(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglQueryNativeWindowNV(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglQueryOutputLayerAttribEXT(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglQueryOutputLayerStringEXT(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglQueryOutputPortAttribEXT(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglQueryOutputPortStringEXT(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglQueryStreamAttribKHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglQueryStreamKHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglQueryStreamMetadataNV(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglQueryStreamTimeKHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglQueryStreamu64KHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglQueryString(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglQuerySurface(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglQuerySurface64KHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglQuerySurfacePointerANGLE(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglReleaseTexImage(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglReleaseThread(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglResetStreamNV(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglSetBlobCacheFuncsANDROID(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglSetDamageRegionKHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglSetStreamAttribKHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglSetStreamMetadataNV(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglSignalSyncKHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglSignalSyncNV(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglStreamAttribKHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglStreamConsumerAcquireAttribKHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglStreamConsumerAcquireKHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglStreamConsumerGLTextureExternalAttribsNV(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglStreamConsumerGLTextureExternalKHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglStreamConsumerOutputEXT(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglStreamConsumerReleaseAttribKHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglStreamConsumerReleaseKHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglStreamFlushNV(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglSurfaceAttrib(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglSwapBuffers(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglSwapBuffersRegion2NOK(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglSwapBuffersRegionNOK(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglSwapBuffersWithDamageEXT(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglSwapBuffersWithDamageKHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglSwapInterval(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglTerminate(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglUnlockSurfaceKHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglUnsignalSyncEXT(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglWaitClient(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglWaitGL(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglWaitNative(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglWaitSync(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
void *eglWaitSyncKHR(void) __attribute__((alias("__likeos_egl_stub"), visibility("default")));
