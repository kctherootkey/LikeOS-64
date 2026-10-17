/*
 * The "likeosaudio" GStreamer plugin: registers likeosaudiosink, the output
 * to the LikeOS audio service.  Installed as
 * /usr/lib/gstreamer-1.0/libgstlikeosaudio.so; GStreamer derives the plugin
 * name from that file name, so the two must stay in step.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>

#include "gstlikeosaudiosink.h"

#ifndef VERSION
#define VERSION "1.0"
#endif
/* GST_PLUGIN_DEFINE records PACKAGE as the plugin's source module. */
#ifndef PACKAGE
#define PACKAGE "gst-likeos-audio"
#endif
#ifndef GST_PACKAGE_NAME
#define GST_PACKAGE_NAME "LikeOS GStreamer audio output"
#endif
#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "https://likeos.systemtrap.com/"
#endif

static gboolean plugin_init(GstPlugin *plugin)
{
	/* Nothing else lives in this plugin.  Registration fails only when
	 * the type cannot be created, and then the plugin is useless. */
	return GST_ELEMENT_REGISTER(likeosaudiosink, plugin);
}

/* The licence string must be one GStreamer recognises, or the registry
 * refuses the plugin; the project's code is GPL version 2. */
GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, likeosaudio,
		  "Audio output through the LikeOS audio service (audiod)",
		  plugin_init, VERSION, "GPL", GST_PACKAGE_NAME,
		  GST_PACKAGE_ORIGIN)
