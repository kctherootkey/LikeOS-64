#!/usr/bin/perl
#
# gen-keymaps.pl - the console keymaps, made from the X keyboard descriptions
#
# Usage: gen-keymaps.pl CATALOGUE XKBDIR COMPOSE KEYSYMDEF OUTDIR
#
#   CATALOGUE  res/keyboard/layouts.tsv (id, name, language, layout, variant)
#   XKBDIR     the xkeyboard-config tree the image ships
#              (build/xorg-sysroot/usr/share/X11/xkb)
#   COMPOSE    the X compose table of the UTF-8 locale
#              (build/xorg-sysroot/usr/share/X11/locale/en_US.UTF-8/Compose)
#   KEYSYMDEF  X11/keysymdef.h of the same tree, for keysym -> Unicode
#   OUTDIR     receives <id>.map for every catalogue line, the alias links,
#              and nonlatin.list
#
# Runs on the BUILD HOST at image-build time.  The console and the X session
# are meant to type the same characters, so the console keymaps are not a
# second, hand-kept collection: each one is translated from the very XKB
# layout the X server loads for the same catalogue line, by console-setup's
# ckbcomp (host/ckbcomp, kept unmodified beside its licence).  Every input is
# pinned: the XKB data and the compose table come out of the X.Org port's
# checksummed tarballs, and ckbcomp is a file in this tree.
#
# What is added to ckbcomp's output, and why:
#
#  * A Latin layout for the scripts that have none.  A console keymap holding
#    only Cyrillic, Greek, Arabic, Thai ... letters cannot type a user name,
#    a password or a command.  Such a layout is generated as the pair
#    "us,<layout>": the US layout is active when the map is loaded, Alt+Shift
#    switches to the national one and back (AltGr_Lock in the map), exactly
#    the arrangement such a system's installer sets up.  The ids concerned
#    are written to nonlatin.list, where likeos-xkb-apply finds them to give
#    the X session the same way back to Latin letters.
#
#  * Dead keys that work.  ckbcomp writes the dead keys but, in UTF-8, no
#    accent table for them, which leaves dead_diaeresis + u to whatever
#    table the kernel happens to hold.  Each map therefore carries its own
#    compose lines, taken from the X compose table for exactly the dead keys
#    the map contains: the console composes what an X program composes.
#    The kernel's table holds 256 entries, so when a map's dead keys have
#    more sequences than fit, the ones a console user is likeliest to need
#    are kept (accent + space for the bare accent, then accented Latin
#    letters, then the rest).
#
# What is taken away:
#
#  * Key codes above 255.  ckbcomp emits a few for keys the console has no
#    use for (Braille dot keys, the currency keys of some keyboards); a
#    console keymap is indexed by an 8-bit key code, so they cannot be loaded
#    anyway and only make a map that the loader rejects or truncates.
#  * Anything ckbcomp appends from the build host's own console-setup
#    configuration (/etc/console-setup/remap.inc): the image must not depend
#    on what happens to be installed on the machine that built it.
#
# Copyright (C) 2026 The LikeOS Project

use strict;
use warnings;

@ARGV == 5 or die "usage: $0 CATALOGUE XKBDIR COMPOSE KEYSYMDEF OUTDIR\n";
my ($catalogue, $xkbdir, $compose_file, $keysymdef, $outdir) = @ARGV;

my $here = $0;
$here =~ s|/[^/]*$|| or $here = '.';
my $ckbcomp = "$here/ckbcomp/ckbcomp";
-f $ckbcomp or die "$0: $ckbcomp: not found\n";
-d "$xkbdir/rules" or die "$0: $xkbdir: no XKB rules directory\n";

# ---------------------------------------------------------------------------
# The dead keys a console keymap can hold, by their X name: the console's
# name for each (as ckbcomp writes it), and the accent character the kernel
# uses for it in the compose table.  The order is the console's dead-key
# numbering, and the characters are the ones that numbering stands for.
# ---------------------------------------------------------------------------
my @dead = (
	[ 'dead_grave',              'dead_grave',              '`'  ],
	[ 'dead_acute',              'dead_acute',              "'"  ],
	[ 'dead_circumflex',         'dead_circumflex',         '^'  ],
	[ 'dead_tilde',              'dead_tilde',              '~'  ],
	[ 'dead_diaeresis',          'dead_diaeresis',          '"'  ],
	[ 'dead_cedilla',            'dead_cedilla',            ','  ],
	[ 'dead_macron',             'dead_macron',             '_'  ],
	[ 'dead_breve',              'dead_kbreve',             'U'  ],
	[ 'dead_abovedot',           'dead_abovedot',           '.'  ],
	[ 'dead_abovering',          'dead_abovering',          '*'  ],
	[ 'dead_doubleacute',        'dead_kdoubleacute',       '='  ],
	[ 'dead_caron',              'dead_kcaron',             'c'  ],
	[ 'dead_ogonek',             'dead_kogonek',            'k'  ],
	[ 'dead_iota',               'dead_iota',               'i'  ],
	[ 'dead_voiced_sound',       'dead_voiced_sound',       '#'  ],
	[ 'dead_semivoiced_sound',   'dead_semivoiced_sound',   'o'  ],
	[ 'dead_belowdot',           'dead_belowdot',           '!'  ],
	[ 'dead_hook',               'dead_hook',               '?'  ],
	[ 'dead_horn',               'dead_horn',               '+'  ],
	[ 'dead_stroke',             'dead_stroke',             '-'  ],
	[ 'dead_abovecomma',         'dead_abovecomma',         ')'  ],
	[ 'dead_abovereversedcomma', 'dead_abovereversedcomma', '('  ],
	[ 'dead_doublegrave',        'dead_doublegrave',        ':'  ],
	[ 'dead_invertedbreve',      'dead_invertedbreve',      'n'  ],
	[ 'dead_belowcomma',         'dead_belowcomma',         ';'  ],
	[ 'dead_currency',           'dead_currency',           '$'  ],
	[ 'dead_greek',              'dead_greek',              '@'  ],
);
my %x_to_console = map { $_->[0] => $_->[1] } @dead;
my %console_accent = map { $_->[1] => $_->[2] } @dead;
# X spells a few of them two ways.
$x_to_console{'dead_psili'} = 'dead_abovecomma';
$x_to_console{'dead_dasia'} = 'dead_abovereversedcomma';
$x_to_console{'dead_perispomeni'} = 'dead_tilde';

# The kernel's table has 256 entries, and the loader reports a full table as
# "too many compose definitions"; one short of full loads silently.
my $MAX_COMPOSE = 255;

# The accented letters each common dead key is there for -- the repertoire
# of the classic console keymaps.  They go first when the table is full.
my %preferred = (
	'dead_grave'        => 'AEIOUaeiou',
	'dead_macron'       => 'AEIOUaeiou',
	'dead_acute'        => 'ACEILNORSUYZaceilnorsuyz',
	'dead_circumflex'   => 'ACEGHIJOSUWYaceghijosuwy',
	'dead_tilde'        => 'AINOUainou',
	'dead_diaeresis'    => 'AEIOUYaeiouy',
	'dead_abovering'    => 'AUau',
	'dead_cedilla'      => 'ACEGKLNRSTacegklnrst',
	'dead_kcaron'       => 'CDELNRSTZcdelnrstz',
	'dead_kogonek'      => 'AEIUaeiu',
	'dead_kbreve'       => 'AGUagu',
	'dead_kdoubleacute' => 'OUou',
	'dead_abovedot'     => 'CEGIZcegz',
);

# The names the classic console keymap collection gives these layouts, as
# links to the catalogue map they stand for.  An alias is created only when
# its target is in the catalogue, and never shadows a catalogue id.
my %aliases = (
	'uk'                    => 'gb',
	'de-latin1'             => 'de',
	'de-latin1-nodeadkeys'  => 'de-nodeadkeys',
	'neo'                   => 'de-neo',
	'fr-latin1'             => 'fr',
	'fr-latin9'             => 'fr',
	'fr-pc'                 => 'fr',
	'azerty'                => 'fr-azerty',
	'be-latin1'             => 'be',
	'sg'                    => 'ch',
	'sg-latin1'             => 'ch',
	'de_CH-latin1'          => 'ch',
	'cf'                    => 'ca',
	'dk-latin1'             => 'dk',
	'no-latin1'             => 'no',
	'sv-latin1'             => 'se',
	'is-latin1'             => 'is',
	'la-latin1'             => 'latam',
	'pt-latin1'             => 'pt',
	'pt-latin9'             => 'pt',
	'br-abnt2'              => 'br',
	'nl2'                   => 'nl',
	'croat'                 => 'hr',
	'slovene'               => 'si',
	'sk-qwertz'             => 'sk',
	'pl2'                   => 'pl',
	'hu101'                 => 'hu',
	'trq'                   => 'tr',
	'tr_q-latin5'           => 'tr',
	'trf'                   => 'tr-f',
	'gr-pc'                 => 'gr',
	'bg_bds-utf8'           => 'bg',
	'bg_pho-utf8'           => 'bg-phonetic',
	'ruwin_alt-UTF-8'       => 'ru',
	'sr-cy'                 => 'rs',
	'mk-utf'                => 'mk',
	'jp106'                 => 'jp',
	'dvorak'                => 'us-dvorak',
	'dvorak-uk'             => 'gb-dvorak',
	'us-acentos'            => 'us-intl',
);

# ---------------------------------------------------------------------------
# Inputs
# ---------------------------------------------------------------------------

# keysym name -> Unicode code point, from the comments of keysymdef.h.
my %keysym;
open(my $ks, '<', $keysymdef) or die "$0: $keysymdef: $!\n";
while (<$ks>) {
	next unless /^#define XK_(\w+)\s+0x[0-9a-fA-F]+\s*\/\*[ (]U\+([0-9A-Fa-f]{4,6})\b/;
	$keysym{$1} = hex($2) unless exists $keysym{$1};
}
close($ks);
$keysym{'space'} = 0x20;
sub keysym_unicode {
	my ($name) = @_;
	return hex($1) if $name =~ /^U([0-9A-Fa-f]{4,6})$/;
	return $keysym{$name};
}

# <dead_x> <base> : "R" sequences of the X compose table.
my @sequences;
open(my $cf, '<:encoding(UTF-8)', $compose_file) or die "$0: $compose_file: $!\n";
while (<$cf>) {
	next unless /^<(dead_\w+)>\s+<(\w+)>\s*:\s*"((?:[^"\\]|\\.)*)"/;
	my ($dk, $base, $res) = ($1, $2, $3);
	my $console = $x_to_console{$dk} or next;
	$res =~ s/\\(.)/$1/g;
	next unless length($res) == 1;
	my $b = keysym_unicode($base);
	next unless defined $b;
	# A console keymap holds code points below U+F000 only: the values
	# above are how it marks a key's value as Unicode in the first place.
	next if $b >= 0xf000 || ord($res) >= 0xf000;
	push @sequences, [ $console, $b, ord($res), $dk ];
}
close($cf);

my @catalogue;
open(my $cat, '<', $catalogue) or die "$0: $catalogue: $!\n";
while (<$cat>) {
	chomp;
	next if /^\s*(#|$)/;
	my @f = split(/\t/, $_, -1);
	@f == 5 or die "$0: $catalogue:$.: expected 5 fields\n";
	$f[0] =~ /^[a-z0-9_-]+$/ or die "$0: $catalogue:$.: bad id '$f[0]'\n";
	push @catalogue, \@f;
}
close($cat);

# The xkeyboard-config version, for the header of every map.
my $xkbversion = 'unknown';
# Found beside the data by name, not through "..": the X11/xkb directory is
# a link into xkeyboard-config-2, so ".." of it is not the share directory.
(my $sharedir = $xkbdir) =~ s|/+X11/+xkb/*$||;
if (open(my $pc, '<', "$sharedir/pkgconfig/xkeyboard-config.pc")) {
	while (<$pc>) { $xkbversion = $1 if /^Version:\s*(\S+)/; }
	close($pc);
}

# ---------------------------------------------------------------------------
# Translation
# ---------------------------------------------------------------------------

sub ckbcomp {
	my ($layout, $variant, $option) = @_;
	my @cmd = ('perl', $ckbcomp, "-I$xkbdir", '-rules', 'evdev',
		   '-model', 'pc105', '-compact',
		   '-layout', $layout, '-variant', $variant);
	push @cmd, ('-option', $option) if $option ne '';
	my $pid = open(my $out, '-|');
	defined $pid or die "$0: fork: $!\n";
	if ($pid == 0) {
		# ckbcomp's warnings are about multimedia keysyms that have no
		# console equivalent; they are noise here.
		open(STDERR, '>', '/dev/null');
		exec(@cmd) or exit 127;
	}
	local $/;
	my $text = <$out>;
	close($out);
	$? == 0 or die "$0: ckbcomp failed for $layout($variant)\n";
	return $text;
}

# Does the map's unshifted main block type the Latin alphabet?
sub is_latin {
	my ($text) = @_;
	my %seen;
	for my $line (split /\n/, $text) {
		next unless $line =~ /^keycode\s+(\d+)\s*=\s*(\S+)/;
		next unless $1 >= 2 && $1 <= 53;
		my $s = $2;
		$s =~ s/^\+//;
		if ($s =~ /^U\+([0-9a-fA-F]{4})$/) {
			my $c = hex($1);
			$seen{$c} = 1 if $c >= 0x61 && $c <= 0x7a;
		} elsif ($s =~ /^[a-z]$/) {
			$seen{ord($s)} = 1;
		}
	}
	return scalar(keys %seen) >= 20;
}

sub cchar {
	my ($c) = @_;
	return "'\\''" if $c == 0x27;
	return "'\\\\'" if $c == 0x5c;
	return sprintf("'%c'", $c) if $c >= 0x20 && $c < 0x7f;
	return sprintf('U+%04x', $c);
}

sub compose_lines {
	my ($text) = @_;
	my (%dead_in_map, %reachable);
	for my $line (split /\n/, $text) {
		next unless $line =~ /^keycode\s+\d+\s*=(.*)$/;
		for my $s (split ' ', $1) {
			$s =~ s/^\+//;
			$dead_in_map{$s} = 1 if exists $console_accent{$s};
			$reachable{hex($1)} = 1 if $s =~ /^U\+([0-9a-fA-F]{4,6})$/;
		}
	}
	$reachable{0x20} = 1;

	my (%have, @cand);
	for my $q (@sequences) {
		my ($console, $base, $res, $xname) = @$q;
		next unless $dead_in_map{$console} && $reachable{$base};
		my $accent = ord($console_accent{$console});
		next if $have{"$accent:$base"}++;
		my $score = 0;
		if ($base == 0x20) {
			$score += 3000;
		} else {
			$score += 250 if $base >= 0x61 && $base <= 0x7a;
			$score += 200 if $base >= 0x41 && $base <= 0x5a;
			$score -= 200 if $base >= 0x30 && $base <= 0x39;
			$score -= 50 if $base > 0x7a;
			$score += 300 if $preferred{$console} &&
				$base < 0x80 && index($preferred{$console}, chr($base)) >= 0;
			$score += $res <= 0xff ? 200 : $res <= 0x17f ? 100 : 25;
			$score -= 100 if $reachable{$res};
		}
		push @cand, [ $score, $accent, $base, $res, $console ];
	}
	@cand = sort { $b->[0] <=> $a->[0] || $a->[1] <=> $b->[1] ||
		       $a->[2] <=> $b->[2] } @cand;
	splice(@cand, $MAX_COMPOSE) if @cand > $MAX_COMPOSE;
	@cand = sort { $a->[4] cmp $b->[4] || $a->[2] <=> $b->[2] } @cand;
	return map {
		sprintf("compose %s %s to U+%04x\t# %s", cchar($_->[1]),
			cchar($_->[2]), $_->[3], $_->[4])
	} @cand;
}

sub clean {
	my ($text) = @_;
	my @out;
	for my $line (split /\n/, $text) {
		next if $line =~ /^\s*(?:\w+\s+)*keycode\s+(\d+)\s*=/ && $1 > 255;
		push @out, $line;
		last if $line =~ /^strings as usual\s*$/;
	}
	return @out;
}

# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

-d $outdir or mkdir($outdir) or die "$0: $outdir: $!\n";
my (%ids, @nonlatin);
for my $c (@catalogue) {
	my ($id, $name, $lang, $layout, $variant) = @$c;
	$ids{$id} = 1;
	my $text = ckbcomp($layout, $variant, '');
	my $pair = '';
	if (!is_latin($text)) {
		$text = ckbcomp("us,$layout", ",$variant", 'grp:alt_shift_toggle');
		$pair = 1;
		push @nonlatin, $id;
	}
	my @body = clean($text);
	my @compose = compose_lines($text);
	my $xkb = $variant ne '' ? "$layout($variant)" : $layout;

	open(my $o, '>:encoding(UTF-8)', "$outdir/$id.map.tmp")
		or die "$0: $outdir/$id.map.tmp: $!\n";
	print $o "# $id - $name\n";
	print $o "#\n";
	print $o "# Console keymap made from the XKB layout $xkb of xkeyboard-config\n";
	print $o "# $xkbversion by host/gen-keymaps.pl (ckbcomp).  Generated: edit the\n";
	print $o "# layout or the generator, not this file.\n";
	if ($pair) {
		print $o "#\n";
		print $o "# The layout has no Latin letters, so it comes paired with the US\n";
		print $o "# layout, which is active when the map is loaded: Alt+Shift switches\n";
		print $o "# between the two.\n";
	}
	print $o "\n";
	print $o "$_\n" for @body;
	if (@compose) {
		print $o "\n# Dead keys: the X compose table's sequences for this map's dead keys.\n";
		print $o "$_\n" for @compose;
	}
	close($o) or die "$0: $outdir/$id.map.tmp: $!\n";
	rename("$outdir/$id.map.tmp", "$outdir/$id.map")
		or die "$0: $outdir/$id.map: $!\n";
}

for my $a (sort keys %aliases) {
	my $t = $aliases{$a};
	next if $ids{$a};
	die "$0: alias $a: no catalogue map $t\n" unless $ids{$t};
	unlink("$outdir/$a.map");
	symlink("$t.map", "$outdir/$a.map") or die "$0: $outdir/$a.map: $!\n";
}

open(my $nl, '>', "$outdir/nonlatin.list") or die "$0: $outdir/nonlatin.list: $!\n";
print $nl "# Catalogue ids whose layout has no Latin letters (host/gen-keymaps.pl).\n";
print $nl "$_\n" for @nonlatin;
close($nl);

printf "keymaps: %d maps (%d paired with us), %d aliases\n",
	scalar(@catalogue), scalar(@nonlatin),
	scalar(grep { !$ids{$_} } keys %aliases);
