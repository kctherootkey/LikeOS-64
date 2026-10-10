-------------------------------------------------------------------------------
-- LikeOS luakit theme: the Windows 11 light look, to match the rest of the
-- desktop (the GTK theme, the window titles and the taskbar).
--
-- Installed as /etc/xdg/luakit/theme.lua, over the upstream default (white on
-- black).  A user's own ~/.config/luakit/theme.lua still wins.
--
-- The colours are Windows 11's: the Mica window colour #f3f3f3 for the tab
-- strip and the status bar, white for the selected tab (a card joined to the
-- page, as Edge draws it), #1b1b1b text, the light-blue list selection
-- #cce8ff, and the accent blue for progress and links.  The interface font is
-- Selawik, the open substitute for Segoe UI, at Windows' 9 points (12 px).
--
-- Copyright (C) 2026 The LikeOS Project
-------------------------------------------------------------------------------

local theme = {}

-- Default settings
theme.font = "12px Selawik"
theme.fg   = "#1b1b1b"
theme.bg   = "#f3f3f3"

-- General colours
theme.success_fg = "#0f7b0f"
theme.loaded_fg  = "#005fb8"
theme.error_fg = "#ffffff"
theme.error_bg = "#c42b1c"

-- Warning colours
theme.warning_fg = "#9d5d00"
theme.warning_bg = "#fff4ce"

-- Notification colours
theme.notif_fg = "#1b1b1b"
theme.notif_bg = "#ffffff"

-- Menu colours (completion lists, bookmarks, history, ...)
theme.menu_fg                   = "#1b1b1b"
theme.menu_bg                   = "#f9f9f9"
theme.menu_selected_fg          = "#1b1b1b"
theme.menu_selected_bg          = "#cce8ff"
theme.menu_title_bg             = "#f3f3f3"
theme.menu_primary_title_fg     = "#005fb8"
theme.menu_secondary_title_fg   = "#616161"

theme.menu_disabled_fg = "#a0a0a0"
theme.menu_disabled_bg = theme.menu_bg
theme.menu_enabled_fg = theme.menu_fg
theme.menu_enabled_bg = theme.menu_bg
theme.menu_active_fg = "#0f7b0f"
theme.menu_active_bg = theme.menu_bg

-- Proxy manager
theme.proxy_active_menu_fg      = "#1b1b1b"
theme.proxy_active_menu_bg      = "#ffffff"
theme.proxy_inactive_menu_fg    = "#8a8a8a"
theme.proxy_inactive_menu_bg    = "#ffffff"

-- Statusbar specific
theme.sbar_fg         = "#1b1b1b"
theme.sbar_bg         = "#f3f3f3"

-- Downloadbar specific
theme.dbar_fg         = "#1b1b1b"
theme.dbar_bg         = "#f3f3f3"
theme.dbar_error_fg   = "#c42b1c"

-- Input bar specific
theme.ibar_fg           = "#1b1b1b"
theme.ibar_bg           = "rgba(0,0,0,0)"

-- Tab label: unselected tabs are grey text on the strip, washed a shade
-- darker under the pointer; the selected one is a white card with dark text.
theme.tab_fg            = "#5c5c5c"
theme.tab_bg            = "#f3f3f3"
theme.tab_hover_bg      = "#e9e9e9"
theme.tab_ntheme        = "#8a8a8a"
theme.selected_fg       = "#1b1b1b"
theme.selected_bg       = "#ffffff"
theme.selected_ntheme   = "#616161"
theme.loading_fg        = "#0078d4"
theme.loading_bg        = "#ffffff"

theme.selected_private_tab_bg = "#ece4f7"
theme.private_tab_bg    = "#f3eefa"

-- Trusted/untrusted ssl colours
theme.trust_fg          = "#0f7b0f"
theme.notrust_fg        = "#c42b1c"

-- Follow mode hints
theme.hint_font = "11px monospace, courier, sans-serif"
theme.hint_fg = "#ffffff"
theme.hint_bg = "#005fb8"
theme.hint_border = "1px solid #003e92"
theme.hint_opacity = "0.3"
theme.hint_overlay_bg = "rgba(204,232,255,0.4)"
theme.hint_overlay_border = "1px dotted #005fb8"
theme.hint_overlay_selected_bg = "rgba(15,123,15,0.3)"
theme.hint_overlay_selected_border = theme.hint_overlay_border

-- General colour pairings
theme.ok = { fg = "#1b1b1b", bg = "#ffffff" }
theme.warn = { fg = "#9d5d00", bg = "#fff4ce" }
theme.error = { fg = "#ffffff", bg = "#c42b1c" }

-- Gopher page style (override defaults)
theme.gopher_light = { bg = "#ffffff", fg = "#1b1b1b", link = "#005fb8" }
theme.gopher_dark  = { bg = "#202020", fg = "#e8e8e8", link = "#60cdff" }

return theme
