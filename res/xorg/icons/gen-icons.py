#!/usr/bin/env python3
# LikeOS -- generate the LikeOS icon theme (res/xorg/icons/LikeOS).
#
# Copyright (C) 2026 The LikeOS Project
#
# LikeOS has no SVG loader: gdk-pixbuf on the target carries no librsvg (it
# would bring a Rust toolchain with it), so every icon the system draws must
# already be a PNG.  This script rasterizes the theme's SVG artwork on the
# build host and writes the PNG theme that is committed to the repository,
# the same way gtk.css is generated from the theme's SCSS and committed.  Run
# it again after changing a table below or a drawing; the output is
# deterministic, so unchanged input reproduces byte-identical files.
#
# Sources:
#
#   LikeOS artwork, res/xorg/icons/src/<context>/*.svg
#     Every full-colour icon: folders, documents, devices, places,
#     applications, dialog symbols, the LikeOS mark and the start orb.
#     Drawn by draw-icons.py (run that first after changing a drawing);
#     each icon comes as a master drawing for 48 pixels and up and as -32,
#     -24 and -16 variants fitted to those pixel grids (the start orb only
#     as the master: it is installed at 48 and 96 pixels).
#
#   Fluent UI System Icons by Microsoft, MIT
#     https://github.com/microsoft/fluentui-system-icons
#     npm package @fluentui/svg-icons 1.1.345, downloaded by --fetch into
#     the --sources directory.  The line ("regular") icons, which become
#     the likeos-* command icons and the thin replacements for GTK's own
#     symbolic glyphs.  Licence: LikeOS/LICENSE.fluentui-system-icons,
#     shipped with the theme.
#
# What comes out: full-colour places, devices, mime types, applications and
# dialog symbols at 16 to 256 pixels under their freedesktop names (plus the
# usual alternative spellings as symbolic links), and the command glyphs at
# 16, 20, 24, 32 and 48 pixels as likeos-<name> in the theme's ink colour,
# the freedesktop action names (edit-copy, go-previous, ...) linked to them,
# and likeos-<name>-symbolic, the same glyph for GTK to paint in the CSS
# colour, and thin replacements for GTK's own symbolic glyphs (see SYMBOLIC).
#
# Usage:
#   gen-icons.py --fetch --sources DIR     download the line icons into DIR
#   gen-icons.py --sources DIR [--out res/xorg/icons/LikeOS]
#
# Host requirements: Python 3 with PyGObject (GdkPixbuf 2.0, whose SVG loader
# is librsvg) and Pillow.  Nothing here runs on LikeOS itself.

import argparse
import hashlib
import io
import os
import re
import shutil
import sys
import tarfile
import urllib.request

import gi
gi.require_version('GdkPixbuf', '2.0')
from gi.repository import GdkPixbuf  # noqa: E402
from PIL import Image  # noqa: E402

ART = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'src')
FLUENT_URL = ('https://registry.npmjs.org/@fluentui/svg-icons/-/'
	      'svg-icons-1.1.345.tgz')
FLUENT_SHA256 = ('6610366b1eb5ce4ce70c95d11accbf88'
		 '58b01ec81c71d31bc6493586ac8cc905')
FLUENT_LICENSE_URL = ('https://raw.githubusercontent.com/microsoft/'
		      'fluentui-system-icons/main/LICENSE')

# The sizes every full-colour icon is drawn at.  16 to 48 are what
# GtkIconSize maps to (MENU and BUTTON 16, LARGE_TOOLBAR 24, DND 32, DIALOG
# 48) plus the 22 panels ask for; 64 to 256 are the file manager's medium,
# large and extra-large icon views, its panes and Home.
COLOUR_SIZES = [16, 22, 24, 32, 48, 64, 96, 128, 256]
# The command icons: 16 in menus and command bars, 20 and 24 in title bars
# and toolbars, 32 for the few large buttons (the media player's transport).
ACTION_SIZES = [16, 20, 24, 32, 48]
# (48 is for the media player's large transport buttons and anything else
# that asks a command glyph for more than 32 pixels; without it GTK would
# scale the 32-pixel image up and blur its strokes.)

# The LikeOS ink (the light palette's primary text colour) and accent,
# used for the accent half of the two-tone commands.
INK = '#141a24'
ACCENT = '#2a5bd7'


# ---------------------------------------------------------------------------
# Source specifications.
#
#   A(path)          LikeOS artwork: src/<path>.svg, or its -16, -24 or -32
#                    variant for the sizes those are drawn for.
#   FL(name, colour, filled=False, accent=None, inset=False)
#                    Line icon recoloured; accent names a region
#                    ('right' or 'centre') drawn in the accent colour.
#                    inset draws the glyph at three quarters of the size,
#                    centred, for the small glyphs of caption buttons and
#                    drop-down chevrons.
# ---------------------------------------------------------------------------
class A:
	def __init__(self, path):
		self.path = path


class FL:
	def __init__(self, name, colour=INK, filled=False, accent=None,
		     inset=False):
		self.name = name
		self.colour = colour
		self.filled = filled
		self.accent = accent
		self.inset = inset


# Each entry: name -> (spec, [aliases]).  An alias is a relative symlink in
# the same directory, so every common spelling of a name resolves without a
# second copy of the image.
PLACES = {
	'folder': (A('places/folder'),
		   ['inode-directory', 'folder-blue', 'gnome-fs-directory',
		    'default-folder']),
	'folder-open': (A('places/folder-open'),
			['folder-drag-accept', 'folder-visiting',
			 'inode-directory-open']),
	'folder-documents': (A('places/folder-documents'),
			     ['folder-document', 'folder_documents']),
	'folder-download': (A('places/folder-download'),
			    ['folder-downloads', 'folder_downloads']),
	'folder-music': (A('places/folder-music'),
			 ['folder-sound', 'folder_music']),
	'folder-pictures': (A('places/folder-pictures'),
			    ['folder-images', 'folder-image', 'folder_pictures',
			     'folder-picture']),
	'folder-videos': (A('places/folder-videos'),
			  ['folder-video', 'folder_videos', 'folder-movies']),
	'folder-templates': (A('places/folder-templates'),
			     ['folder_templates']),
	'folder-publicshare': (A('places/folder-publicshare'),
			       ['folder-public', 'folder_publicshare']),
	'folder-remote': (A('places/folder-remote'),
			  ['folder-network', 'folder-remote-smb',
			   'folder-remote-ftp', 'folder-remote-ssh',
			   'folder-remote-nfs', 'gnome-fs-smb']),
	'user-desktop': (A('places/user-desktop'),
			 ['desktop', 'folder-desktop', 'gnome-fs-desktop']),
	'user-home': (A('places/user-home'),
		      ['folder-home', 'gnome-fs-home']),
	'user-trash': (A('places/user-trash'),
		       ['trash-empty', 'gnome-fs-trash-empty',
			'user-trash-empty', 'emptytrash']),
	'user-trash-full': (A('places/user-trash-full'),
			    ['trash-full', 'gnome-fs-trash-full']),
	'network-workgroup': (A('places/network-workgroup'),
			      ['network', 'gnome-fs-network',
			       'likeos-network-color']),
	'network-server': (A('places/network-server'),
			   ['gnome-fs-server', 'server']),
	# The file manager's Home and Gallery pages.
	'go-home': (A('places/go-home'), ['likeos-home-color',
					  'user-home-page']),
	'likeos-gallery-color': (A('places/gallery'), ['folder-gallery']),
	'user-bookmarks': (A('places/user-bookmarks'),
			   ['bookmarks', 'favorites', 'starred',
			    'likeos-favorite-color']),
	'document-open-recent': (A('places/document-open-recent'),
				 ['folder-recent', 'likeos-recent-color']),
}

DEVICES = {
	'computer': (A('devices/computer'),
		     ['gnome-dev-computer', 'system', 'video-display',
		      'likeos-computer-color', 'likeos-this-pc-color',
		      'computer-desktop']),
	'computer-laptop': (A('devices/computer-laptop'), ['laptop']),
	'drive-harddisk': (A('devices/drive-harddisk'),
			   ['drive-harddisk-system', 'drive-harddisk-root',
			    'drive-harddisk-solidstate', 'drive-harddisk-ieee1394',
			    'drive-multidisk', 'gnome-dev-harddisk',
			    'drive-harddisk-scsi']),
	'drive-removable-media': (A('devices/drive-removable-media'),
				  ['drive-removable-media-usb',
				   'drive-harddisk-usb', 'media-removable',
				   'drive-removable-media-usb-pendrive',
				   'gnome-dev-removable', 'media-flash',
				   'media-flash-sd-mmc']),
	'drive-optical': (A('devices/drive-optical'),
			  ['drive-cdrom', 'gnome-dev-cdrom']),
	'media-optical': (A('devices/media-optical'),
			  ['media-cdrom', 'media-optical-cd', 'media-optical-dvd',
			   'media-optical-data', 'gnome-dev-disc-cdrom']),
	'printer': (A('devices/printer'), []),
	'input-keyboard': (A('devices/input-keyboard'), []),
	'input-mouse': (A('devices/input-mouse'), []),
	'audio-speakers': (A('devices/audio-speakers'), ['audio-card']),
	'audio-headphones': (A('devices/audio-headphones'), ['audio-headset']),
	'camera-photo': (A('devices/camera-photo'), []),
	'phone': (A('devices/phone'), []),
}

MIMETYPES = {
	'text-x-generic': (A('mimetypes/text-x-generic'),
			   ['text-plain', 'gnome-mime-text', 'text-x-log',
			    'text-x-authors', 'text-x-changelog',
			    'text-x-install']),
	'image-x-generic': (A('mimetypes/image-x-generic'),
			    ['gnome-mime-image', 'image']),
	'video-x-generic': (A('mimetypes/video-x-generic'),
			    ['gnome-mime-video', 'video']),
	'audio-x-generic': (A('mimetypes/audio-x-generic'),
			    ['gnome-mime-audio', 'audio']),
	'application-pdf': (A('mimetypes/application-pdf'),
			    ['gnome-mime-application-pdf']),
	'package-x-generic': (A('mimetypes/package-x-generic'),
			      ['application-x-archive', 'application-zip',
			       'application-x-zip',
			       'application-x-zip-compressed',
			       'application-x-tar', 'application-x-compressed-tar',
			       'application-x-bzip-compressed-tar',
			       'application-x-xz-compressed-tar',
			       'application-x-zstd-compressed-tar',
			       'application-x-lzma-compressed-tar',
			       'application-x-7z-compressed', 'application-x-rar',
			       'application-vnd.rar', 'application-gzip',
			       'application-x-gzip', 'application-x-bzip',
			       'application-x-bzip2', 'application-x-xz',
			       'application-zstd', 'application-x-cpio',
			       'application-x-ar', 'archive']),
	'application-x-executable': (A('mimetypes/application-x-executable'),
				     ['application-x-pie-executable',
				      'gnome-fs-executable', 'exec',
				      'application-x-elf']),
	'application-x-sharedlib': (A('mimetypes/application-x-sharedlib'),
				    []),
	'application-x-object': (A('mimetypes/application-x-object'), []),
	'text-html': (A('mimetypes/text-html'),
		      ['application-xhtml+xml', 'text-x-html']),
	'application-x-shellscript': (A('mimetypes/application-x-shellscript'),
				      ['text-x-script', 'application-x-perl',
				       'text-x-sh']),
	'x-office-document': (A('mimetypes/x-office-document'),
			      ['application-msword', 'application-rtf',
			       'application-vnd.oasis.opendocument.text',
			       'application-vnd.openxmlformats-officedocument.'
			       'wordprocessingml.document',
			       'text-rtf']),
	'x-office-spreadsheet': (A('mimetypes/x-office-spreadsheet'),
				 ['application-vnd.ms-excel', 'text-csv',
				  'application-vnd.oasis.opendocument.'
				  'spreadsheet',
				  'application-vnd.openxmlformats-'
				  'officedocument.spreadsheetml.sheet']),
	'x-office-presentation': (A('mimetypes/x-office-presentation'),
				  ['application-vnd.ms-powerpoint',
				   'application-vnd.oasis.opendocument.'
				   'presentation',
				   'application-vnd.openxmlformats-'
				   'officedocument.presentationml.'
				   'presentation']),
	'application-x-desktop': (A('mimetypes/application-x-desktop'),
				  ['application-x-ms-shortcut',
				   'application-x-link']),
	# A file of unknown type is a blank page.
	'unknown': (A('mimetypes/unknown'),
		    ['application-x-generic', 'empty']),
	'application-octet-stream': (A('mimetypes/application-octet-stream'),
				     ['application-x-firmware']),
	'text-x-csrc': (A('mimetypes/text-x-csrc'),
			['text-x-c', 'text-x-chdr', 'text-x-c++src',
			 'text-x-c++hdr', 'text-x-cpp']),
	'text-x-python': (A('mimetypes/text-x-python'),
			  ['text-x-python3', 'application-x-python']),
	'text-x-makefile': (A('mimetypes/text-x-makefile'), ['text-x-cmake']),
	'text-markdown': (A('mimetypes/text-markdown'), ['text-x-markdown']),
	'text-x-readme': (A('mimetypes/text-x-readme'), ['text-x-copying']),
	'application-json': (A('mimetypes/application-json'),
			     ['application-x-yaml', 'text-x-yaml']),
	'text-xml': (A('mimetypes/text-xml'),
		     ['application-xml', 'text-x-xml', 'text-css',
		      'text-javascript', 'application-javascript',
		      'text-x-javascript']),
	'text-x-patch': (A('mimetypes/text-x-patch'), ['text-x-diff']),
	'font-x-generic': (A('mimetypes/font-x-generic'),
			   ['font-ttf', 'font-otf', 'application-x-font-ttf']),
	'application-x-cd-image': (A('mimetypes/application-x-cd-image'),
				   ['application-x-iso9660-image',
				    'application-vnd.efi.iso']),
	'x-office-calendar': (A('mimetypes/x-office-calendar'),
			      ['text-calendar']),
}

APPS = {
	# LikeOS Explorer and every other file manager name.
	'system-file-manager': (A('apps/system-file-manager'),
				['file-manager', 'likeos-explorer',
				 'org.xfce.filemanager', 'pcmanfm',
				 'org.gnome.Nautilus']),
	'utilities-terminal': (A('apps/utilities-terminal'),
			       ['org.xfce.terminal', 'terminal',
				'org.xfce.terminal-settings', 'xterm',
				'gnome-terminal']),
	'accessories-calculator': (A('apps/accessories-calculator'),
				   ['galculator', 'calc',
				    'org.gnome.Calculator']),
	'accessories-text-editor': (A('apps/accessories-text-editor'),
				    ['org.xfce.mousepad', 'mousepad',
				     'text-editor']),
	'internet-mail': (A('apps/internet-mail'),
			  ['claws-mail', 'mail-client', 'email']),
	'web-browser': (A('apps/web-browser'),
			['internet-web-browser', 'luakit', 'browser']),
	'internet-chat': (A('apps/internet-chat'),
			  ['io.github.Hexchat', 'hexchat', 'irc-chat']),
	'multimedia-player': (A('apps/multimedia-player'),
			      ['video-player', 'likeos-mediaplayer',
			       'multimedia-video-player', 'media-player']),
	'image-viewer': (A('apps/image-viewer'),
			 ['accessories-image-viewer']),
	'preferences-system': (A('apps/preferences-system'),
			       ['preferences-desktop', 'systemsettings',
				'gnome-control-center', 'applications-system']),
	'preferences-desktop-display': (A('apps/preferences-desktop-display'),
					['video-display-settings',
					 'likeos-display-settings']),
	'preferences-desktop-wallpaper': (A('apps/'
					    'preferences-desktop-wallpaper'),
					  ['background']),
	'avatar-default': (A('apps/avatar-default'),
			   ['user-info', 'system-users', 'stock_person']),
	# The LikeOS mark: About boxes, start-here, the distributor logo.
	'likeos-logo': (A('apps/likeos-logo'),
			['start-here', 'distributor-logo', 'likeos-start',
			 'likeos', 'start-here-likeos']),
}

STATUS = {
	'dialog-warning': (A('status/dialog-warning'), ['gtk-dialog-warning']),
	'dialog-error': (A('status/dialog-error'), ['gtk-dialog-error']),
	'dialog-question': (A('status/dialog-question'),
			    ['gtk-dialog-question']),
	'dialog-information': (A('status/dialog-information'),
			       ['gtk-dialog-info', 'dialog-info']),
}

# The mark as a symbolic icon, for GTK to paint in the CSS colour:
# src/apps/likeos-logo-symbolic, at the command icon sizes.
LOGO_SYMBOLIC = 'apps/likeos-logo-symbolic'

# The panel's start orb, one image per state, drawn for a 48-pixel panel
# (a 44-pixel orb centred in 48, the margin for the hover halo) and for a
# display at twice the scale: 48x48/apps/<name>.png and 96x96/apps/<name>.png.
# The panel cross-fades between them; nothing scales them.
ORB = ['likeos-start-orb', 'likeos-start-orb-hover',
       'likeos-start-orb-pressed']
ORB_SIZES = [48, 96]

# The command icons: likeos-<name> -> (line icon, accent region, aliases).
# Aliases here are the freedesktop names of the same commands, so menus and
# toolbars of every GTK program draw the same glyphs too.  Only the
# full-colour names are aliased; the -symbolic names GTK recolours itself
# are the SYMBOLIC table below.
ACTIONS = [
	('new', 'add_circle', 'centre', []),
	('new-folder', 'folder_add', None, ['folder-new']),
	('new-file', 'document_add', None, ['document-new']),
	('new-tab', 'tab_add', None, ['tab-new']),
	('new-window', 'window_new', None, ['window-new']),
	('cut', 'cut', None, ['edit-cut']),
	('copy', 'copy', None, ['edit-copy']),
	('paste', 'clipboard_paste', None, ['edit-paste']),
	('rename', 'rename', None, ['edit-rename']),
	('share', 'share', None, ['emblem-shared-action', 'document-send']),
	('delete', 'delete', None, ['edit-delete', 'user-trash-action']),
	('delete-permanently', 'delete_dismiss', None, []),
	('restore', 'arrow_reset', None, []),
	('empty-trash', 'delete_lines', None, []),
	('sort', 'arrow_sort', 'right', ['view-sort-ascending',
					 'view-sort-descending']),
	('view', 'list', None, []),
	('filter', 'filter', None, []),
	('more', 'more_horizontal', None, ['view-more', 'open-menu']),
	('details-pane', 'panel_right', None, []),
	('preview-pane', 'panel_right_contract', None, []),
	('back', 'arrow_left', None, ['go-previous']),
	('forward', 'arrow_right', None, ['go-next']),
	('up', 'arrow_up', None, ['go-up']),
	('down', 'arrow_down', None, ['go-down']),
	('refresh', 'arrow_clockwise', None, ['view-refresh']),
	('search', 'search', None, ['system-search', 'edit-find']),
	('pin', 'pin', None, ['view-pin']),
	('unpin', 'pin_off', None, []),
	('open', 'open', None, ['document-open']),
	('open-folder', 'folder_open', None, []),
	('open-with', 'window_arrow_up', None, []),
	('properties', 'wrench', None, ['document-properties']),
	('copy-path', 'clipboard_link', None, []),
	('copy-to', 'folder_arrow_right', None, []),
	('move-to', 'folder_arrow_left', None, []),
	('create-shortcut', 'link', None, ['insert-link']),
	('compress', 'folder_zip', None, []),
	('extract', 'folder_arrow_up', None, []),
	('wallpaper', 'image', None, []),
	('rotate-left', 'arrow_rotate_counterclockwise', None,
	 ['object-rotate-left']),
	('rotate-right', 'arrow_rotate_clockwise', None,
	 ['object-rotate-right']),
	('favorite', 'star', None, []),
	('terminal', 'window_console', None, []),
	('select-all', 'select_all_on', None, ['edit-select-all']),
	('select-none', 'select_all_off', None, []),
	('invert-selection', 'multiselect_ltr', None, []),
	('undo', 'arrow_undo', None, ['edit-undo']),
	('redo', 'arrow_redo', None, ['edit-redo']),
	('close', 'dismiss', None, ['window-close', 'edit-clear']),
	('chevron-right', 'chevron_right', None, []),
	('chevron-left', 'chevron_left', None, []),
	('chevron-down', 'chevron_down', None, []),
	('chevron-up', 'chevron_up', None, []),
	('home', 'home', None, []),
	('gallery', 'image_multiple', None, []),
	('computer', 'desktop', None, []),
	('this-pc', 'desktop', None, []),
	('network', 'globe', None, []),
	('drive', 'hard_drive', None, []),
	('folder', 'folder', None, []),
	('document', 'document', None, []),
	('desktop', 'desktop', None, []),
	('downloads', 'arrow_download', None, []),
	('documents', 'document_text', None, []),
	('pictures', 'image', None, []),
	('music', 'music_note_2', None, []),
	('videos', 'video', None, []),
	('recent', 'history', None, []),
	('trash', 'delete', None, []),
	('list-view', 'list', None, ['view-list']),
	('details-view', 'apps_list_detail', None, []),
	('tiles-view', 'apps_list', None, []),
	('content-view', 'content_view', None, []),
	('icons-view', 'grid', None, ['view-grid']),
	('large-icons-view', 'square_multiple', None, []),
	('play', 'play', None, ['media-playback-start']),
	('pause', 'pause', None, ['media-playback-pause']),
	('stop', 'stop', None, ['media-playback-stop']),
	('previous', 'previous', None, ['media-skip-backward']),
	('next', 'next', None, ['media-skip-forward']),
	('skip-back-10', 'skip_back_10', None, ['media-seek-backward']),
	('skip-forward-10', 'skip_forward_10', None, ['media-seek-forward']),
	('shuffle', 'arrow_shuffle', None, ['media-playlist-shuffle']),
	('repeat', 'arrow_repeat_all', None, ['media-playlist-repeat']),
	('repeat-one', 'arrow_repeat_1', None,
	 ['media-playlist-repeat-song']),
	('playlist', 'text_bullet_list_ltr', None, []),
	('volume', 'speaker_2', None, ['audio-volume-high']),
	('volume-medium', 'speaker_1', None, ['audio-volume-medium']),
	('volume-low', 'speaker_0', None, ['audio-volume-low']),
	('volume-mute', 'speaker_mute', None, ['audio-volume-muted']),
	('fullscreen', 'full_screen_maximize', None, ['view-fullscreen']),
	('leave-fullscreen', 'full_screen_minimize', None, []),
	('closed-captions', 'closed_caption', None, []),
	('cancel', 'dismiss_circle', None, ['process-stop']),
	('eject', 'arrow_eject', None, ['media-eject']),
	('info', 'info', None, ['help-about']),
	('help', 'question_circle', None, ['help-contents', 'help-browser']),
	('warning', 'warning', None, []),
	('check', 'checkmark', None, ['object-select']),
	('settings', 'settings', None, []),
	('power', 'power', None, ['system-shutdown']),
	('restart', 'arrow_sync_circle', None, ['system-reboot']),
	('sleep', 'weather_moon', None, ['system-suspend']),
	('lock', 'lock_closed', None, ['system-lock-screen']),
	('sign-out', 'sign_out', None, ['system-log-out']),
	('user', 'person', None, []),
	('apps', 'apps', None, []),
	('all-apps', 'grid_dots', None, []),
	('add', 'add', None, ['list-add']),
	('remove', 'subtract', None, ['list-remove']),
	('edit', 'edit', None, []),
	('save', 'save', None, ['document-save']),
	('save-as', 'save_edit', None, ['document-save-as']),
	('print', 'print', None, ['document-print']),
	('send', 'send', None, ['mail-send']),
	('zoom-in', 'zoom_in', None, ['zoom-in']),
	('zoom-out', 'zoom_out', None, ['zoom-out']),
	('menu', 'navigation', None, []),
	('maximize', 'maximize', None, []),
	('minimize', 'subtract', None, []),
	('window-restore', 'square_multiple', None, []),
	('mail', 'mail', None, []),
	('globe', 'globe', None, []),
	('clock', 'clock', None, []),
	('calendar', 'calendar', None, []),
	('wifi', 'wifi_1', None, []),
	('battery', 'battery_10', None, []),
	('notification', 'alert', None, []),
	('tag', 'tag', None, []),
	('cloud', 'cloud', None, []),
	('image-edit', 'image_edit', None, []),
	('slideshow', 'slide_play', None, []),
	# The media player's queue and add-a-folder buttons.
	('play-queue', 'navigation_play', None, []),
	('folder-add', 'folder_add', None, []),
]


# GTK's own symbolic icons, replaced where a widget draws one that the theme's
# CSS cannot: the chevrons of menu buttons, combo boxes, sorted column
# headers, path bars and expanders, the spin buttons' plus and minus, the
# caption buttons, the search and clear glyphs of entries, the file chooser's
# places, the media controls.  Adwaita's are filled and heavy; these are the
# thin line icons, as a GTK symbolic PNG painted in the CSS colour.
#   (name, line icon, line icon for right-to-left or None, inset)
SYMBOLIC = [
	('pan-down-symbolic', 'chevron_down', None, True),
	('pan-up-symbolic', 'chevron_up', None, True),
	('pan-start-symbolic', 'chevron_left', 'chevron_right', True),
	('pan-end-symbolic', 'chevron_right', 'chevron_left', True),
	('list-add-symbolic', 'add', None, True),
	('list-remove-symbolic', 'subtract', None, True),
	('value-increase-symbolic', 'add', None, True),
	('value-decrease-symbolic', 'subtract', None, True),
	('window-close-symbolic', 'dismiss', None, True),
	('window-minimize-symbolic', 'subtract', None, True),
	('window-maximize-symbolic', 'square', None, True),
	('window-restore-symbolic', 'square_multiple', None, True),
	('edit-clear-symbolic', 'dismiss', None, True),
	('edit-find-symbolic', 'search', None, False),
	('system-search-symbolic', 'search', None, False),
	('open-menu-symbolic', 'navigation', None, False),
	('view-more-symbolic', 'more_horizontal', None, False),
	('view-more-horizontal-symbolic', 'more_horizontal', None, False),
	('go-previous-symbolic', 'arrow_left', 'arrow_right', False),
	('go-next-symbolic', 'arrow_right', 'arrow_left', False),
	('go-up-symbolic', 'arrow_up', None, False),
	('go-down-symbolic', 'arrow_down', None, False),
	('go-home-symbolic', 'home', None, False),
	('object-select-symbolic', 'checkmark', None, False),
	('process-stop-symbolic', 'dismiss_circle', None, False),
	('document-open-symbolic', 'folder_open', None, False),
	('document-save-symbolic', 'save', None, False),
	('document-new-symbolic', 'document_add', None, False),
	('folder-symbolic', 'folder', None, False),
	('folder-open-symbolic', 'folder_open', None, False),
	('folder-new-symbolic', 'folder_add', None, False),
	('user-trash-symbolic', 'delete', None, False),
	('emblem-system-symbolic', 'settings', None, False),
	('view-list-symbolic', 'list', None, False),
	('view-grid-symbolic', 'grid', None, False),
	('view-pin-symbolic', 'pin', None, False),
	('view-refresh-symbolic', 'arrow_clockwise', None, False),
	('view-sort-ascending-symbolic', 'arrow_sort_up', None, False),
	('view-sort-descending-symbolic', 'arrow_sort_down', None, False),
	('view-fullscreen-symbolic', 'full_screen_maximize', None, False),
	('view-restore-symbolic', 'full_screen_minimize', None, False),
	('edit-copy-symbolic', 'copy', None, False),
	('edit-cut-symbolic', 'cut', None, False),
	('edit-paste-symbolic', 'clipboard_paste', None, False),
	('edit-delete-symbolic', 'delete', None, False),
	('edit-undo-symbolic', 'arrow_undo', 'arrow_redo', False),
	('edit-redo-symbolic', 'arrow_redo', 'arrow_undo', False),
	('edit-select-all-symbolic', 'select_all_on', None, False),
	('edit-clear-all-symbolic', 'delete_lines', None, False),
	('insert-link-symbolic', 'link', None, False),
	('starred-symbolic', 'star', None, False),
	('non-starred-symbolic', 'star', None, False),
	('dialog-warning-symbolic', 'warning', None, False),
	('dialog-information-symbolic', 'info', None, False),
	('dialog-question-symbolic', 'question_circle', None, False),
	('dialog-error-symbolic', 'error_circle', None, False),
	('audio-volume-high-symbolic', 'speaker_2', None, False),
	('audio-volume-medium-symbolic', 'speaker_1', None, False),
	('audio-volume-low-symbolic', 'speaker_0', None, False),
	('audio-volume-muted-symbolic', 'speaker_mute', None, False),
	('media-playback-start-symbolic', 'play', None, False),
	('media-playback-pause-symbolic', 'pause', None, False),
	('media-playback-stop-symbolic', 'stop', None, False),
	('media-skip-forward-symbolic', 'next', None, False),
	('media-skip-backward-symbolic', 'previous', None, False),
	('media-seek-forward-symbolic', 'fast_forward', None, False),
	('media-seek-backward-symbolic', 'rewind', None, False),
	('media-playlist-shuffle-symbolic', 'arrow_shuffle', None, False),
	('media-playlist-repeat-symbolic', 'arrow_repeat_all', None, False),
	('media-playlist-repeat-song-symbolic', 'arrow_repeat_1', None, False),
	('media-eject-symbolic', 'arrow_eject', None, False),
	('user-home-symbolic', 'home', None, False),
	('user-desktop-symbolic', 'desktop', None, False),
	('folder-documents-symbolic', 'document_text', None, False),
	('folder-download-symbolic', 'arrow_download', None, False),
	('folder-music-symbolic', 'music_note_2', None, False),
	('folder-pictures-symbolic', 'image', None, False),
	('folder-videos-symbolic', 'video', None, False),
	('document-open-recent-symbolic', 'history', None, False),
	('drive-harddisk-symbolic', 'hard_drive', None, False),
	('computer-symbolic', 'desktop', None, False),
	('network-workgroup-symbolic', 'globe', None, False),
	('audio-x-generic-symbolic', 'music_note_2', None, False),
	('video-x-generic-symbolic', 'video', None, False),
	('image-x-generic-symbolic', 'image', None, False),
	('text-x-generic-symbolic', 'document', None, False),
	('preferences-system-symbolic', 'settings', None, False),
	('system-shutdown-symbolic', 'power', None, False),
	('system-lock-screen-symbolic', 'lock_closed', None, False),
	('avatar-default-symbolic', 'person', None, False),
	('mail-send-symbolic', 'send', None, False),
	('tab-new-symbolic', 'tab_add', None, False),
	('zoom-in-symbolic', 'zoom_in', None, False),
	('zoom-out-symbolic', 'zoom_out', None, False),
]


# ---------------------------------------------------------------------------
# Fetching the line icons.
# ---------------------------------------------------------------------------
def fetch(sources):
	os.makedirs(sources, exist_ok=True)
	fdir = os.path.join(sources, 'fluentui-svg-icons')
	if not os.path.isdir(os.path.join(fdir, 'package')):
		os.makedirs(fdir, exist_ok=True)
		data = urllib.request.urlopen(FLUENT_URL).read()
		got = hashlib.sha256(data).hexdigest()
		if got != FLUENT_SHA256:
			sys.exit('gen-icons: %s has sha256 %s, expected %s' %
				 (FLUENT_URL, got, FLUENT_SHA256))
		with tarfile.open(fileobj=io.BytesIO(data)) as t:
			for m in t.getmembers():
				# Plain files under package/ only: no links, no
				# absolute or parent-relative names.
				if not m.isreg() and not m.isdir():
					continue
				if m.name.startswith('/') or '..' in m.name:
					continue
				t.extract(m, fdir)
		lic = urllib.request.urlopen(FLUENT_LICENSE_URL).read()
		with open(os.path.join(fdir, 'LICENSE'), 'wb') as f:
			f.write(lic)


# ---------------------------------------------------------------------------
# Rasterizing.
# ---------------------------------------------------------------------------
class Sources:
	def __init__(self, root):
		self.fl = os.path.join(root, 'fluentui-svg-icons', 'package',
				       'icons')
		for d in (self.fl, ART):
			if not os.path.isdir(d):
				sys.exit('gen-icons: missing %s (run --fetch, '
					 'draw-icons.py)' % d)

	def art_file(self, path, size):
		# The variant drawn for this pixel grid: -16 for 16, -24 for
		# 20 to 24, -32 for 32, the master drawing above that.
		suffix = ('-16' if size <= 16 else '-24' if size <= 24 else
			  '-32' if size <= 32 else '')
		p = os.path.join(ART, path + suffix + '.svg')
		if not os.path.isfile(p):
			sys.exit('gen-icons: drawing %s missing (run '
				 'draw-icons.py)' % p)
		return p

	def line_file(self, name, size, style):
		have = []
		for z in (16, 20, 24, 28, 32, 48):
			p = os.path.join(self.fl, '%s_%d_%s.svg' % (name, z, style))
			if os.path.isfile(p):
				have.append((z, p))
		if not have:
			sys.exit('gen-icons: line icon %s (%s) not found' %
				 (name, style))
		# The design drawn for exactly this size, or else the nearest
		# larger one (scaling down keeps strokes crisper than scaling
		# up), or else the largest there is.
		for z, p in have:
			if z == size:
				return p
		bigger = [h for h in have if h[0] > size]
		return bigger[0][1] if bigger else have[-1][1]


def svg_to_image(data, size):
	"""Rasterize SVG bytes into a size x size RGBA image, aspect kept."""
	loader = GdkPixbuf.PixbufLoader.new_with_type('svg')

	def prepared(ld, w, h):
		s = size / max(w, h)
		ld.set_size(max(1, round(w * s)), max(1, round(h * s)))
	loader.connect('size-prepared', prepared)
	loader.write(data)
	loader.close()
	pb = loader.get_pixbuf()
	if not pb.get_has_alpha():
		pb = pb.add_alpha(False, 0, 0, 0)
	im = Image.frombytes('RGBA', (pb.get_width(), pb.get_height()),
			     pb.get_pixels(), 'raw', 'RGBA', pb.get_rowstride())
	out = Image.new('RGBA', (size, size), (0, 0, 0, 0))
	out.paste(im, ((size - im.width) // 2, (size - im.height) // 2))
	return out


def recolour(data, colour):
	# The line icons carry no fill: they draw in the default black.
	# Setting fill on the root element colours every path.
	text = data.decode('utf-8')
	text = re.sub(r'<svg ', '<svg fill="%s" ' % colour, text, count=1)
	text = text.replace('currentColor', colour)
	return text.encode('utf-8')


def accent(im, region):
	"""Recolour one region of a glyph in the accent colour."""
	r, g, b = (int(ACCENT[i:i + 2], 16) for i in (1, 3, 5))
	px = im.load()
	w, h = im.size
	for y in range(h):
		for x in range(w):
			a = px[x, y][3]
			if not a:
				continue
			if region == 'right':
				inside = x >= w // 2
			else:
				# The middle of a ringed symbol: the plus in a
				# circle.  Radius 30 percent of the icon.
				dx = x + 0.5 - w / 2
				dy = y + 0.5 - h / 2
				inside = dx * dx + dy * dy <= (0.30 * w) ** 2
			if inside:
				px[x, y] = (r, g, b, a)
	return im


def render(src, spec, size):
	if isinstance(spec, A):
		with open(src.art_file(spec.path, size), 'rb') as f:
			return svg_to_image(f.read(), size)
	if isinstance(spec, FL) and spec.inset:
		d = round(size * 0.75)
		im = render(src, FL(spec.name, spec.colour, spec.filled,
				    spec.accent), d)
		out = Image.new('RGBA', (size, size), (0, 0, 0, 0))
		out.paste(im, ((size - d) // 2, (size - d) // 2))
		return out
	if isinstance(spec, FL):
		style = 'filled' if spec.filled else 'regular'
		with open(src.line_file(spec.name, size, style), 'rb') as f:
			im = svg_to_image(recolour(f.read(), spec.colour), size)
		if spec.accent:
			im = accent(im, spec.accent)
		return im
	raise TypeError(spec)


def save(im, path):
	im.save(path, 'PNG', optimize=True)


def link(directory, alias, target):
	p = os.path.join(directory, alias + '.png')
	if os.path.lexists(p):
		os.remove(p)
	os.symlink(target + '.png', p)


# ---------------------------------------------------------------------------
# The theme.
# ---------------------------------------------------------------------------
INDEX_HEAD = """\
[Icon Theme]
Name=LikeOS
Comment=LikeOS icons (generated by gen-icons.py)
Inherits=Adwaita,hicolor
Example=folder
"""

CONTEXT_NAMES = {
	'actions': 'Actions', 'apps': 'Applications', 'devices': 'Devices',
	'mimetypes': 'MimeTypes', 'places': 'Places', 'status': 'Status',
}


def build(src, out):
	if os.path.isdir(out):
		# Only the size directories are regenerated; index.theme and
		# the licence files are rewritten below.
		for d in os.listdir(out):
			if re.match(r'^\d+x\d+$', d):
				shutil.rmtree(os.path.join(out, d))
	os.makedirs(out, exist_ok=True)
	dirs = []
	seen = {}
	tables = [('places', PLACES), ('devices', DEVICES),
		  ('mimetypes', MIMETYPES), ('apps', APPS), ('status', STATUS)]
	for ctx, table in tables:
		for size in COLOUR_SIZES:
			d = '%dx%d/%s' % (size, size, ctx)
			path = os.path.join(out, d)
			os.makedirs(path, exist_ok=True)
			dirs.append((d, size, ctx))
			for name, (spec, aliases) in table.items():
				save(render(src, spec, size),
				     os.path.join(path, name + '.png'))
				for a in aliases:
					link(path, a, name)
		for name, (_, aliases) in table.items():
			for n in [name] + aliases:
				if n in seen:
					sys.exit('gen-icons: %s twice (%s, %s)' %
						 (n, seen[n], ctx))
				seen[n] = ctx
	for size in ORB_SIZES:
		path = os.path.join(out, '%dx%d/apps' % (size, size))
		for name in ORB:
			save(render(src, A('orb/' + name), size),
			     os.path.join(path, name + '.png'))
	for name in ORB:
		if name in seen:
			sys.exit('gen-icons: %s twice (orb)' % name)
		seen[name] = 'apps'
	for size in ACTION_SIZES:
		d = '%dx%d/actions' % (size, size)
		path = os.path.join(out, d)
		os.makedirs(path, exist_ok=True)
		dirs.append((d, size, 'actions'))
		for name, line, acc, aliases in ACTIONS:
			spec = FL(line, INK, accent=acc)
			save(render(src, spec, size),
			     os.path.join(path, 'likeos-' + name + '.png'))
			for a in aliases:
				link(path, a, 'likeos-' + name)
			# The same glyph as a symbolic icon: GTK reads a
			# "-symbolic.symbolic.png" as a mask and paints its
			# black pixels in the widget's CSS colour, so the
			# theme can draw it white on an accent button, light
			# on a dark surface, or dimmed when insensitive.
			save(render(src, FL(line, '#000000'), size),
			     os.path.join(path, 'likeos-%s-symbolic.symbolic.png'
					  % name))
		# GTK's own symbolic glyphs, in the thin line.
		for name, line, rtl, inset in SYMBOLIC:
			save(render(src, FL(line, '#000000', inset=inset),
				    size),
			     os.path.join(path, name + '.symbolic.png'))
			if rtl:
				save(render(src, FL(rtl, '#000000',
						    inset=inset), size),
				     os.path.join(path,
						  name + '-rtl.symbolic.png'))
		# The mark, drawn black for GTK to paint (see LOGO_SYMBOLIC).
		save(render(src, A(LOGO_SYMBOLIC), size),
		     os.path.join(path, 'likeos-logo-symbolic.symbolic.png'))
	seen['likeos-logo-symbolic'] = 'actions'
	for name, _, rtl, _ in SYMBOLIC:
		for n in [name] + ([name + '-rtl'] if rtl else []):
			if n in seen:
				sys.exit('gen-icons: %s twice (symbolic)' % n)
			seen[n] = 'actions'
	for name, _, _, aliases in ACTIONS:
		for n in ['likeos-' + name, 'likeos-%s-symbolic' % name] + aliases:
			if n in seen:
				sys.exit('gen-icons: %s twice (%s, actions)' %
					 (n, seen[n]))
			seen[n] = 'actions'

	# The line icons' licence travels with them, as it requires; files
	# of earlier sources that no longer contribute are removed.
	shutil.copyfile(os.path.join(os.path.dirname(src.fl), '..', 'LICENSE'),
			os.path.join(out, 'LICENSE.fluentui-system-icons'))
	for stale in ('COPYING.GPL-3.0', 'AUTHORS.Win11-icon-theme'):
		if os.path.exists(os.path.join(out, stale)):
			os.remove(os.path.join(out, stale))

	dirs.sort(key=lambda t: (t[1], t[2]))
	with open(os.path.join(out, 'index.theme'), 'w') as f:
		f.write(INDEX_HEAD)
		f.write('Directories=%s\n' % ','.join(d for d, _, _ in dirs))
		for d, size, ctx in dirs:
			# Fixed: each directory holds exactly one size, and
			# GTK picks the nearest one and scales it for any
			# other request.
			f.write('\n[%s]\nSize=%d\nContext=%s\nType=Fixed\n' %
				(d, size, CONTEXT_NAMES[ctx]))
	print('gen-icons: %d names in %d directories written to %s' %
	      (len(seen), len(dirs), out))


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument('--sources', required=True)
	ap.add_argument('--fetch', action='store_true')
	ap.add_argument('--out', default=os.path.join(
		os.path.dirname(os.path.abspath(__file__)), 'LikeOS'))
	a = ap.parse_args()
	if a.fetch:
		fetch(a.sources)
		return
	build(Sources(a.sources), a.out)


if __name__ == '__main__':
	main()
