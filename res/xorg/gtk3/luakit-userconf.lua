-- luakit's user configuration for this image.
--
-- The one setting worth reading before anything else is the
-- hardware-acceleration policy further down: it is chosen at startup from
-- whether this machine has a GPU the kernel could bind, so the same image is
-- correct on a machine with one and on a machine without.

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

-- Hardware acceleration: DECIDED BY THE MACHINE, at startup.
--
-- WebKitGTK 2.44+ has two real rendering modes for this toolkit: "always"
-- runs every page through the accelerated compositor, "never" paints with
-- cairo.  ("on-demand" is accepted and treated as "always".)  Which one is
-- right depends entirely on what is underneath:
--
--   With a GPU -- the kernel bound a display-manager driver, /dev/dri/card0
--   exists, and Mesa reaches it through the svga driver -- the compositor
--   draws into GPU textures and hands the finished frame over as a buffer
--   descriptor.  That is what WebGL, accelerated 2D canvas and the GPU
--   process all need, and it is faster than cairo on every page.
--
--   Without one, GL is llvmpipe: the compositor becomes several full-screen
--   CPU passes per scroll step (freebsd.org was unscrollable that way) where
--   cairo needs one.  So plain pages take cairo, and the sites that cannot
--   work without a compositor are listed individually.
--
-- The test is the node, because that is the thing that is actually different
-- between the two machines.  Reading it here rather than hardcoding a policy
-- keeps ONE image working correctly on both.
local acceleration = "never"
do
    local node = io.open("/dev/dri/card0", "r")
    if node then
        node:close()
        acceleration = "always"
    end
end
settings.webview.hardware_acceleration_policy = acceleration
if acceleration == "never" then
    -- WebGL needs the compositor, so the pages that are only about WebGL get
    -- it even on the software path -- slowly, but they work.
    settings.on["get.webgl.org"].webview.hardware_acceleration_policy = "always"
end

-- Media: OFF.
--
-- Video is the most expensive thing a page can start on this machine, and it
-- starts without being asked: a decoder thread per stream, decoded frames held
-- in memory, and a compositor pass per frame on top.  None of it is why the
-- browser is here.
--
-- `enable_mediasource' is the one that matters.  Practically every video on
-- the web today is delivered through Media Source Extensions -- the page feeds
-- segments to the player from JavaScript -- so refusing MSE stops the pipeline
-- before it is built rather than after.  A plain progressive <video src=...>
-- file is not covered by it, which is what the gesture setting below is for:
-- such a file will still play, but only when the user clicks it, never on its
-- own.  `enable_webaudio' goes with them; it is the same decode-and-mix work
-- with no picture attached.
--
-- These stay correct after WebKit is rebuilt with -DENABLE_VIDEO=OFF: the
-- properties exist in the API whether or not the feature was compiled in, and
-- setting them then simply has nothing left to switch off.  Until that rebuild
-- happens they are what actually turns media off in this image.
settings.webview.enable_mediasource = false
settings.webview.media_playback_requires_gesture = true
settings.webview.enable_webaudio = false

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
