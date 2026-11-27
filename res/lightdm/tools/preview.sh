#!/bin/sh
# Render the LikeOS sign-in theme headless with Firefox, in every state the
# mock greeter (mock-lightdm.js) can put it in, at a list of screen sizes.
#
#   preview.sh OUTDIR [STATE...]
#
# OUTDIR/site     a copy of the theme with the mock, Selawik and a style that
#                 switches animations off (so a screenshot shows the end state)
# OUTDIR/profile  a throw-away Firefox profile
# OUTDIR/shots    STATE-WxH.png
#
# The theme sizes everything from the viewport (the root font size follows
# vh/vw), so a HiDPI screen at device pixel ratio 2 lays out exactly like
# the same CSS size at ratio 1, only sharper; the 3840x2160 run shows the
# full-resolution case.  (Headless Firefox ignores devPixelsPerPx for
# --screenshot, so ratio 2 cannot be rendered here.)
#
# Environment:
#   SIZES   space-separated WxH list (default: 640x480 up to 3840x2160)
#   RUN     a command prefix for Firefox (e.g. a memory-capped wrapper)
#   FIREFOX the Firefox binary (default: firefox)
#
# Copyright (C) 2026 The LikeOS Project

set -eu

out=${1:?usage: preview.sh OUTDIR [STATE...]}
shift
states=${*:-"lock signin signin-photo other other-typed error error-other empty-user busy power pam-message"}
sizes=${SIZES:-"640x480 800x600 1024x768 1280x720 1280x800 1366x768 1920x1080 2560x1440 3840x2160"}
run=${RUN:-}
firefox=${FIREFOX:-firefox}

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
theme="$here/../theme/likeos"
fonts="$root/ports/xorg/gtk3/Selawik-1.01"

mkdir -p "$out"
out=$(cd "$out" && pwd)
rm -rf "$out/site"
cp -r "$theme" "$out/site"
cp "$here/mock-lightdm.js" "$out/site/js/"
mkdir -p "$out/site/fonts" "$out/shots"
for f in selawk selawkl selawksl selawksb selawkb; do
	cp "$fonts/$f.ttf" "$out/site/fonts/"
done
cat > "$out/site/css/preview.css" <<'EOF'
/* Preview only: Selawik from files (the greeter takes it from the system)
   and no animations, so a screenshot shows the final state. */
@font-face { font-family: "Selawik"; font-weight: 300; src: url(../fonts/selawkl.ttf); }
@font-face { font-family: "Selawik"; font-weight: 350; src: url(../fonts/selawksl.ttf); }
@font-face { font-family: "Selawik"; font-weight: 400; src: url(../fonts/selawk.ttf); }
@font-face { font-family: "Selawik"; font-weight: 600; src: url(../fonts/selawksb.ttf); }
@font-face { font-family: "Selawik"; font-weight: 700; src: url(../fonts/selawkb.ttf); }
*, *::before, *::after { transition: none !important; animation: none !important; }
EOF
sed -i 's|<script src="js/greeter.js"></script>|<link rel="stylesheet" href="css/preview.css">\n<script src="js/mock-lightdm.js"></script>\n<script src="js/greeter.js"></script>|' \
	"$out/site/index.html"

prof="$out/profile"
mkdir -p "$prof"
cat > "$prof/user.js" <<'EOF'
user_pref("browser.shell.checkDefaultBrowser", false);
user_pref("browser.startup.homepage_override.mstone", "ignore");
user_pref("datareporting.policy.dataSubmissionEnabled", false);
user_pref("toolkit.telemetry.reportingpolicy.firstRun", false);
user_pref("security.fileuri.strict_origin_policy", false);
user_pref("gfx.font_rendering.fontconfig.max_generic_substitutions", 3);
EOF

shoot() { # STATE WxH SUFFIX
	w=${2%x*}
	h=${2#*x}
	$run "$firefox" --headless --no-remote --profile "$prof" \
		--window-size="$w,$h" --screenshot "$out/shots/$1-$2$3.png" \
		"file://$out/site/index.html#$1" >/dev/null 2>&1 || true
}

for s in $states; do
	for z in $sizes; do
		shoot "$s" "$z" ""
	done
done

echo "screenshots in $out/shots"
