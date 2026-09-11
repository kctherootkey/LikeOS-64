-- luakit's user configuration for this image.
--
-- The one setting worth reading before anything else is the
-- hardware-acceleration policy further down: it is "always" on every
-- machine, GPU or not, and the comment there says what that costs and how
-- to change it for one machine or one site.

local settings = require "settings"

-- Where a new window starts and how big it opens.  luakit's own defaults are
-- its project page and 800x600; this image opens on the LikeOS site, at a
-- size chosen from the screen the X server is running -- one step below it,
-- so the window fits with the window manager's bar and frame still visible:
--
--     1920x1200, 1920x1080  ->  1280x1024
--     1280x1024, 1280x800   ->  1024x768
--     1024x768              ->   800x600
--
-- The screen size comes from xrandr's header line ("Screen 0: minimum ...,
-- current 1920 x 1080, maximum ..."), which is the server's own answer and
-- needs no support from luakit, which has no screen-size API of its own.
-- Any screen the table does not name gets the largest of the three sizes
-- that leaves room, by width; and if xrandr cannot be asked at all the
-- window opens at 1280x1024, which is what this image used before.
settings.window.home_page = "https://likeos.systemtrap.com"
do
    local size = "1280x1024"
    local sw
    local p = io.popen("xrandr --current 2>/dev/null")
    if p then
        local out = p:read("*a") or ""
        p:close()
        sw = tonumber(out:match("current (%d+) x %d+"))
    end
    if sw then
        if sw >= 1920 then
            size = "1280x1024"
        elseif sw >= 1280 then
            size = "1024x768"
        else
            size = "800x600"
        end
    end
    settings.window.new_window_size = size
end

-- WebGL: ON.  luakit ships it off (webview.enable_webgl defaults to false in
-- lib/webview.lua), so a page asking for a WebGL context is refused before
-- WebKit ever touches EGL -- get.webgl.org says "your browser does not
-- support WebGL" with a perfectly working Mesa underneath.  The GL stack is
-- llvmpipe through EGL (see eglinfo), and WebKit is built with ANGLE on top
-- of it, so there is no reason left to keep it off.  Per-site control stays
-- available through domain_props if a page's shaders are too slow.
settings.webview.enable_webgl = true

-- Hardware acceleration: ALWAYS, on every machine.
--
-- WebKitGTK has two real rendering modes for this toolkit: "always" runs every
-- page through the accelerated compositor, "never" paints with cairo.
-- ("on-demand" is accepted and treated as "always".)  The compositor is what
-- WebGL, the accelerated 2D canvas and CSS 3D transforms need, and it is also
-- what the rest of the engine assumes: with it off, WebKit's non-composited
-- paths -- which upstream barely exercises -- crashed the web process on a
-- null overflow-controls host layer in RenderLayerCompositor.
--
-- This used to be decided by whether /dev/dri/card0 existed, on the grounds
-- that without a GPU the compositor runs on llvmpipe and costs several
-- full-screen CPU passes per frame where cairo needs one (freebsd.org was
-- unscrollable that way).  It is on everywhere now, the ThinkPad on /dev/fb0
-- included, so that both kinds of machine run the same engine paths; the
-- GL underneath is llvmpipe there and the svga driver on a VMware guest with
-- 3D.  Scrolling does not depend on this either way: luakit binds the wheel
-- itself and it never reaches the engine.
--
-- Measured, so that it is not re-discovered from scratch: on the GTK3 API line
-- the accelerated path does NOT end on the GPU even where there is one.
-- WebKit hands the compositor's buffer straight to the toolkit only when its
-- EGL display is the one GTK already uses, and GTK 3.24's X11 backend is
-- GLX-only, so that is never true here -- the UI process CPU-maps the tile
-- every frame and composites it with cairo anyway.  webkit://gpu reports
-- exactly that: "Native interface: None", "Usage: Mapping".
--
-- To go back to cairo for one machine, set this to "never" and export
-- WEBKIT_FORCE_COMPOSITING_MODE=0 before starting luakit (res/xorg/xinitrc
-- explains the variable); a per-user ~/.config/luakit/userconf.lua takes
-- precedence over this file.  Per site, settings.on["domain"] does the same
-- for a page whose layers are too slow on the software path.
settings.webview.hardware_acceleration_policy = "always"

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
