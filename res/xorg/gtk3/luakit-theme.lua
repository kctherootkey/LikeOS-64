-------------------------------------------------------------------------------
-- LikeOS luakit theme: the LikeOS light palette, to match the rest of the
-- desktop (the GTK theme and the window titles).
--
-- Installed as /etc/xdg/luakit/theme.lua, over the upstream default (white on
-- black).  A user's own ~/.config/luakit/theme.lua still wins.
--
-- The tab strip is the title-row colour #eef2f9, the status bar the window
-- colour #f5f7fb, and the selected tab white, joined to the page; text is
-- #141a24 with #586174 for secondary text, completion lists use the list
-- selection blue #dce7fd, and the LikeOS blue marks progress, links and
-- follow-mode hints.  The interface font is Selawik at 12 px (9 points).
--
-- Copyright (C) 2026 The LikeOS Project
-------------------------------------------------------------------------------

local theme = {}

-- Default settings
theme.font = "12px Selawik"
theme.fg   = "#141a24"
theme.bg   = "#f5f7fb"

-- General colours
theme.success_fg = "#17783f"
theme.loaded_fg  = "#2a5bd7"
theme.error_fg = "#ffffff"
theme.error_bg = "#d63a3a"

-- Warning colours
theme.warning_fg = "#9a5f00"
theme.warning_bg = "#fcf3e5"

-- Notification colours
theme.notif_fg = "#141a24"
theme.notif_bg = "#ffffff"

-- Menu colours (completion lists, bookmarks, history, ...)
theme.menu_fg                   = "#141a24"
theme.menu_bg                   = "#ffffff"
theme.menu_selected_fg          = "#141a24"
theme.menu_selected_bg          = "#dce7fd"
theme.menu_title_bg             = "#f5f7fb"
theme.menu_primary_title_fg     = "#2a5bd7"
theme.menu_secondary_title_fg   = "#586174"

theme.menu_disabled_fg = "#a3abb9"
theme.menu_disabled_bg = theme.menu_bg
theme.menu_enabled_fg = theme.menu_fg
theme.menu_enabled_bg = theme.menu_bg
theme.menu_active_fg = "#17783f"
theme.menu_active_bg = theme.menu_bg

-- Proxy manager
theme.proxy_active_menu_fg      = "#141a24"
theme.proxy_active_menu_bg      = "#ffffff"
theme.proxy_inactive_menu_fg    = "#8b94a5"
theme.proxy_inactive_menu_bg    = "#ffffff"

-- Statusbar specific
theme.sbar_fg         = "#141a24"
theme.sbar_bg         = "#f5f7fb"

-- Downloadbar specific
theme.dbar_fg         = "#141a24"
theme.dbar_bg         = "#f5f7fb"
theme.dbar_error_fg   = "#b8292a"

-- Input bar specific
theme.ibar_fg           = "#141a24"
theme.ibar_bg           = "rgba(0,0,0,0)"

-- Tab label: unselected tabs are secondary text on the strip, washed a shade
-- deeper under the pointer; the selected one is white with dark text.
theme.tab_fg            = "#586174"
theme.tab_bg            = "#eef2f9"
theme.tab_hover_bg      = "#e1e7f1"
theme.tab_ntheme        = "#8b94a5"
theme.selected_fg       = "#141a24"
theme.selected_bg       = "#ffffff"
theme.selected_ntheme   = "#586174"
theme.loading_fg        = "#3b74f0"
theme.loading_bg        = "#ffffff"

theme.selected_private_tab_bg = "#efeafb"
theme.private_tab_bg    = "#e6e0f6"

-- Trusted/untrusted ssl colours
theme.trust_fg          = "#17783f"
theme.notrust_fg        = "#b8292a"

-- Follow mode hints
theme.hint_font = "11px monospace, courier, sans-serif"
theme.hint_fg = "#ffffff"
theme.hint_bg = "#2a5bd7"
theme.hint_border = "1px solid #1d47b0"
theme.hint_opacity = "0.3"
theme.hint_overlay_bg = "rgba(220,231,253,0.45)"
theme.hint_overlay_border = "1px dotted #2a5bd7"
theme.hint_overlay_selected_bg = "rgba(31,157,85,0.3)"
theme.hint_overlay_selected_border = theme.hint_overlay_border

-- General colour pairings
theme.ok = { fg = "#141a24", bg = "#ffffff" }
theme.warn = { fg = "#9a5f00", bg = "#fcf3e5" }
theme.error = { fg = "#ffffff", bg = "#d63a3a" }

-- Gopher page style (override defaults)
theme.gopher_light = { bg = "#ffffff", fg = "#141a24", link = "#2251c4" }
theme.gopher_dark  = { bg = "#161c2a", fg = "#eef2fa", link = "#5b8ff9" }

return theme
