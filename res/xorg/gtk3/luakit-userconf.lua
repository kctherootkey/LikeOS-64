-- luakit's user configuration for this image.
--
-- The one setting worth reading before anything else is the
-- hardware-acceleration policy further down: it is "always" on every
-- machine, GPU or not, and the comment there says what that costs and how
-- to change it for one machine or one site.
--
-- Copyright (C) 2026 The LikeOS Project

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

-- A web process of its own for a new site: ON, switchable at run time.
--
-- A tab keeps its web process for every site it is pointed at, and the
-- process keeps what earlier sites left in it.  Seen on the ThinkPad: play
-- a few videos on youtube.com, then load redhat.com in the same tab, and it
-- scrolls in stalls; close the tab (`d', the process exits) and load
-- redhat.com again, and it scrolls as it should.  One reading of that --
-- the process, grown past 1.5 GB by the video, pruning itself every 30 s
-- from then on -- is what this image's WebKit now leaves off by default
-- (WebProcess.cpp in ports/xorg/gtk3/patches/webkitgtk/0013 says how and
-- why).  This setting is the other lever, the one that copies what closing
-- the tab did by hand.  It went in switched off, to be tried on the same
-- image if the first did not hold up; tried on the ThinkPad, it was the
-- better browser -- scrolling stayed smooth and memory stayed down -- so
-- it is on.
--
-- With it on, a load whose site differs from the one this tab first showed
-- -- typed with `o', clicked, or started by a script; not a form
-- submission, a reload or a back/forward step -- opens in a new tab in the
-- same place, and this tab closes.  New tabs (`t', middle click) are never
-- touched: their first load has no earlier site.  The old page leaves
-- history the way `d' loses it; `u' brings the closed tab back.  Frames are not
-- affected: navigation-request fires for every frame, a provisional
-- load-status only for the top one, and the two are matched by address.
-- Two sites are the same when their last two host labels agree
-- (www.redhat.com and access.redhat.com); a two-label public suffix such
-- as co.uk makes distinct sites under it look like one, which only keeps a
-- load in its process, never moves one that should stay.
--
-- Switch it off from the luakit://settings/ page (it is remembered); it is
-- read at each load, so there is nothing to restart.  WebKit's own process
-- swapping (WebKitWebContext:process-swap-on-cross-site-navigation-enabled)
-- was tried first and made scrolling worse even on a fresh redhat.com; it
-- keeps a spare process warm and parks used ones for five minutes, and this
-- does neither -- the old process is gone when the tab is.
settings.register_settings({
    ["webview.fresh_process_per_site"] = {
        type = "boolean",
        default = true,
        desc = "Load a page from another site in a new tab that replaces this one, so that it starts in a web process of its own -- what closing the tab and opening the page again does by hand. Reloads and back/forward stay where they are; the old page leaves history, and `u' brings the tab back.",
    },
})
do
    local webview = require "webview"
    local lousy = require "lousy"

    -- The part of a host that names its site: the last two labels.
    local function site_of(uri)
        if type(uri) ~= "string" or uri == "" then return nil end
        local u = lousy.uri.parse(uri)
        if not u or (u.scheme ~= "http" and u.scheme ~= "https") then return nil end
        local host = (u.host or ""):lower()
        if host == "" then return nil end
        return host:match("([^.]+%.[^.]+)$") or host
    end

    -- Per view: the site it first showed, and the request luakit was last
    -- asked to decide on, with its reason, to match the load that follows.
    local first_site = setmetatable({}, { __mode = "k" })
    local requested = setmetatable({}, { __mode = "k" })

    webview.add_signal("init", function (view)
        view:add_signal("navigation-request", function (v, uri, reason)
            requested[v] = { uri = uri, reason = reason }
        end)
        view:add_signal("load-status", function (v, status)
            if status == "committed" then
                if not first_site[v] then first_site[v] = site_of(v.uri) end
                return
            end
            if status ~= "provisional" then return end
            local req = requested[v]
            requested[v] = nil
            if not settings.get_setting("webview.fresh_process_per_site") then return end
            if not req or req.uri ~= v.uri then return end      -- a frame's, or none
            -- Typed, scripted or clicked only.  A form submission is a
            -- POST, and the tab that replaces this one would GET the
            -- address instead; back/forward and reloads stay put.
            if req.reason ~= "other" and req.reason ~= "link-clicked" then return end
            local from, to = first_site[v], site_of(v.uri)
            if not from or not to or from == to then return end
            local w = webview.window(v)
            if not w then return end
            local uri = v.uri
            -- Not from inside the view's own signal: the tab goes away in
            -- the next main-loop turn, and the load with it.
            luakit.idle_add(function ()
                local idx = w.tabs:indexof(v)
                if not idx then return false end
                w:new_tab(uri, { switch = (w.view == v), order = function () return idx + 1 end })
                w:close_tab(v)
                return false
            end)
        end)
    end)
end

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

-- What a page's JavaScript prints, on the terminal that started X.
--
-- A site that misbehaves here but not on another machine almost always says
-- why in its own console: an exception from a script, a request the network
-- refused, a storage API that would not open.  None of that is visible
-- otherwise -- luakit has no console pane, and the web inspector needs a
-- window of its own.  WebKit can write the same messages to stdout instead,
-- which on this system lands in the terminal `startx' was typed in and in the
-- serial log beside it.
--
-- Off by default because it is every message from every page, including the
-- ones sites print on purpose.  Turn it on for a session with:
--
--     LIKEOS_WEB_CONSOLE=1 startx
--
-- and read the lines that appear while the page that misbehaves loads.
if os.getenv("LIKEOS_WEB_CONSOLE") == "1" then
    settings.webview.enable_write_console_messages_to_stdout = true
end
