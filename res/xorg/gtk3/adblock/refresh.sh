#!/bin/sh
# Fetch the filter lists luakit's ad blocker loads on the image (see the
# staging recipe in the top-level Makefile) into this directory.
#
# EasyList and EasyPrivacy for every site, and on top of them one list per
# language region, so that German, French, Russian, Chinese, ... sites are
# covered by the people who maintain filters for them, plus Peter Lowe's
# ad-server list.  They are snapshots: EasyList marks itself as expiring after
# a few days, so run this before a release build, then rebuild the image.
# The files are redistributed unchanged, under the terms in their own headers.
#
# A list that cannot be fetched, or that comes back as anything but a filter
# list, keeps its previous snapshot (or stays absent).
#
# Copyright (C) 2026 The LikeOS Project

here=$(cd "$(dirname "$0")" && pwd)
failed=0

fetch() {
	name=$1
	url=$2
	if ! curl -sSL --fail --max-time 120 -A "Mozilla/5.0" \
		-o "$here/$name.txt.new" "$url"; then
		echo "$name: download failed, keeping the previous one" >&2
		rm -f "$here/$name.txt.new"
		failed=1
		return
	fi
	# Carriage returns and a byte-order mark go: the parser reads lines
	# as they are.
	tr -d '\r' < "$here/$name.txt.new" | sed '1s/^\xEF\xBB\xBF//' \
		> "$here/$name.txt.clean"
	mv "$here/$name.txt.clean" "$here/$name.txt.new"
	# A filter list is a "[Adblock Plus ...]" header or "!" comments
	# followed by rules; an HTML error page is neither.
	first=$(grep -m1 -v '^[[:space:]]*$' "$here/$name.txt.new")
	case "$first" in
	"["*"Adblock"*|"!"*) ;;
	*)
		echo "$name: not a filter list, keeping the previous one" >&2
		rm -f "$here/$name.txt.new"
		failed=1
		return
		;;
	esac
	mv "$here/$name.txt.new" "$here/$name.txt"
	echo "$name: $(wc -l < "$here/$name.txt") lines"
}

# Every site
fetch easylist          https://easylist.to/easylist/easylist.txt
fetch easyprivacy       https://easylist.to/easylist/easyprivacy.txt
fetch peterlowe         "https://pgl.yoyo.org/adservers/serverlist.php?hostformat=adblockplus&showintro=0&mimetype=plaintext"

# Language regions
fetch easylist-germany  https://easylist.to/easylistgermany/easylistgermany.txt
fetch liste-fr          https://easylist-downloads.adblockplus.org/liste_fr.txt
fetch easylist-italy    https://easylist-downloads.adblockplus.org/easylistitaly.txt
fetch easylist-spanish  https://easylist-downloads.adblockplus.org/easylistspanish.txt
fetch easylist-portuguese https://easylist-downloads.adblockplus.org/easylistportuguese.txt
fetch easylist-dutch    https://easylist-downloads.adblockplus.org/easylistdutch.txt
fetch easylist-polish   https://easylist-downloads.adblockplus.org/easylistpolish.txt
fetch easylist-czech-slovak https://raw.githubusercontent.com/tomasko126/easylistczechandslovak/master/filters.txt
fetch easylist-hebrew   https://raw.githubusercontent.com/easylist/EasyListHebrew/master/EasyListHebrew.txt
fetch liste-ar          https://easylist-downloads.adblockplus.org/Liste_AR.txt
fetch easylist-china    https://easylist-downloads.adblockplus.org/easylistchina.txt
fetch ruadlist          https://easylist-downloads.adblockplus.org/advblock.txt
fetch indianlist        https://easylist-downloads.adblockplus.org/indianlist.txt
fetch bulgarian         https://stanev.org/abp/adblock_bg.txt
fetch latvian           https://raw.githubusercontent.com/Latvian-List/adblock-latvian/master/lists/latvian-list.txt
fetch hufilter          https://cdn.jsdelivr.net/gh/hufilter/hufilter@gh-pages/hufilter.txt
fetch rolist            https://www.zoso.ro/pages/rolist.txt
fetch greek             https://www.void.gr/kargig/void-gr-filters.txt
fetch nordic            https://raw.githubusercontent.com/DandelionSprout/adfilt/master/NorwegianList.txt
fetch abpindo           https://raw.githubusercontent.com/ABPindo/indonesianadblockrules/master/subscriptions/abpindo.txt
fetch abpvn             https://raw.githubusercontent.com/abpvn/abpvn/master/filter/abpvn.txt
fetch youslist-korean   https://raw.githubusercontent.com/yous/YousList/master/youslist.txt
fetch adguard-japanese  https://filters.adtidy.org/extension/ublock/filters/7.txt
fetch adguard-turkish   https://filters.adtidy.org/extension/ublock/filters/13.txt

exit $failed
