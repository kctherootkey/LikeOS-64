-- Hardware-acceleration policy: LEFT AT LUAKIT'S DEFAULT.
--
-- This file once forced "never", because the system had no GL at all -- no
-- Mesa, no libEGL, no DRM -- and WebKit's compositing could only fail.
-- Mesa with llvmpipe now provides real (software) OpenGL through GLX/EGL,
-- and WebKit composites through it, so the override is gone.  To fall back
-- for debugging:
--   settings.override_setting("webview.hardware_acceleration_policy", "never")

local settings = require "settings"

-- Where a new window starts and how big it opens.  luakit's own defaults are
-- its project page and 800x600; this image opens on the LikeOS site at
-- 1024x768, which fits the 1280x800 and larger framebuffers the port runs on
-- with the window manager's bar still visible.
settings.window.home_page = "https://likeos.systemtrap.com"
settings.window.new_window_size = "1024x768"

-- WebGL: ON.  luakit ships it off (webview.enable_webgl defaults to false in
-- lib/webview.lua), so a page asking for a WebGL context is refused before
-- WebKit ever touches EGL -- get.webgl.org says "your browser does not
-- support WebGL" with a perfectly working Mesa underneath.  The GL stack is
-- llvmpipe through EGL (see eglinfo), and WebKit is built with ANGLE on top
-- of it, so there is no reason left to keep it off.  Per-site control stays
-- available through domain_props if a page's shaders are too slow.
settings.webview.enable_webgl = true

-- Hardware acceleration: OFF by default, ON per site.
--
-- WebKitGTK 2.44+ has only two real rendering modes for this toolkit:
-- "always" runs every page through the accelerated compositor, "never" paints
-- with cairo.  ("on-demand" is accepted and treated as "always".)  On this
-- system GL is llvmpipe -- software -- so the compositor means several
-- full-screen CPU passes per scroll step and freebsd.org became unscrollable;
-- cairo needs one pass and is fast.  WebGL requires the compositor, so it is
-- unavailable on "never" pages.  The policy is a per-view setting, so the
-- default is the fast path and sites that need GL are listed below; add
-- your own the same way (or from luakit: `:set webview.hardware_acceleration_policy always`
-- affects the current view only until the next domain change).
settings.webview.hardware_acceleration_policy = "never"
settings.on["get.webgl.org"].webview.hardware_acceleration_policy = "always"

-- Default typed addresses to https://, not http://.
--
-- window.search_open turns what the user typed after `o` into a target.  A
-- bare host ("heise.de") passes lousy.uri.is_uri and comes back UNCHANGED --
-- no scheme -- and WebKit's URL parser then fills in http://.  Wrapping the
-- method here upgrades exactly that case: anything search_open produced that
-- has no scheme of its own.  Everything that already carries one is left
-- alone: explicit http:// stays http:// (typing it remains the escape hatch
-- for the rare plain-http site), and javascript:, file://, luakit:// and the
-- search-engine URLs (all defined with schemes) never match.
--
-- localhost and literal IPs are excluded: those are overwhelmingly local
-- servers where https would only produce a certificate error.
local window = require "window"
local search_open_http = window.methods.search_open
window.methods.search_open = function (w, arg)
    local uri = search_open_http(w, arg)
    if type(uri) == "string"
        and not uri:match("^%a[%w+.-]*:")           -- already has a scheme
        and not uri:match("^localhost[:/]?")
        and not uri:match("^%d+%.%d+%.%d+%.%d+")    -- literal IPv4
    then
        return "https://" .. uri
    end
    return uri
end
