-- luakit's user configuration for this image.
--
-- The one setting worth reading before anything else is the
-- hardware-acceleration policy further down: it is OFF, deliberately, even
-- where the kernel bound a GPU -- because on the GTK3 API line the
-- "accelerated" path ends in a per-frame CPU map that costs more than it
-- saves.  The reasoning is written out there.

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

-- Hardware acceleration: OFF, on every machine, GPU or not.
--
-- WebKitGTK has two real rendering modes for this toolkit: "always" runs
-- every page through the accelerated compositor, "never" paints with cairo.
-- ("on-demand" is accepted and treated as "always".)  This used to be chosen
-- at startup from whether /dev/dri/card0 existed.  It is now "never" either
-- way, and the reason is specific to the GTK3 API line.
--
-- With the compositor on, the accelerated path is NOT "render on the GPU and
-- show it".  It is: the web process renders the tile with the GPU, hands it
-- over as a dma-buf descriptor, and then the UI process CPU-MAPS that buffer
-- and composites from the mapped bytes with cairo.  The map is a full
-- processor/device synchronisation once per frame -- measured at ~6.8ms of
-- every maximized frame on faz.net, more than the whole kernel driver costs.
--
-- WebKit only skips the map when its EGL display is the one GTK already
-- uses, and on GTK3 under X11 that is never true: the sharing path is
-- compiled only for GTK4, and GTK 3.24's X11 backend is GLX-only, so there
-- is no EGL display to share.  webkit://gpu says so directly --
-- "Native interface: None", "Usage: Mapping".  No setting changes it.
--
-- So the comparison is "GPU render + device round trip + CPU map + cairo
-- composite" against plain "cairo", and plain cairo is the shorter path.
-- res/xorg/xinitrc sets WEBKIT_FORCE_COMPOSITING_MODE=0 to match; the two
-- have to stay in step.
--
-- Reverse both the day WebKit is built against GTK4 (webkit2gtk-6.0): the
-- display is shared there, the map disappears, and the accelerated path
-- becomes the shorter one for real.
local acceleration = "never"
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
