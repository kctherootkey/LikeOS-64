#!/bin/bash
#
# testbash.sh - exercise the GNU bash port from a script.
#
# Covers the language and builtins a shell is actually used through:
# expansion, quoting, arithmetic, conditionals, loops, functions, arrays,
# redirection, pipelines, job control, traps and the common builtins.  Not
# every bash feature - the widely used ones, broadly.
#
# Each check prints PASS or FAIL; the exit status is the number of failures.

pass=0
fail=0

ok() {   # ok <description> <actual> <expected>
	if [ "$2" = "$3" ]; then
		printf '  [PASS] %s\n' "$1"
		pass=$((pass + 1))
	else
		printf '  [FAIL] %s\n         got:      [%s]\n         expected: [%s]\n' \
			"$1" "$2" "$3"
		fail=$((fail + 1))
	fi
}

yes_() { # yes_ <description> <command...>  - passes if the command succeeds
	local d=$1; shift
	if "$@"; then
		printf '  [PASS] %s\n' "$d"; pass=$((pass + 1))
	else
		printf '  [FAIL] %s\n' "$d"; fail=$((fail + 1))
	fi
}

no_() {  # no_ <description> <command...>  - passes if the command fails
	local d=$1; shift
	if "$@"; then
		printf '  [FAIL] %s (expected failure)\n' "$d"; fail=$((fail + 1))
	else
		printf '  [PASS] %s\n' "$d"; pass=$((pass + 1))
	fi
}

section() { printf '\n=== %s ===\n' "$1"; }

# [[ ... ]] is a reserved word, not a command, so it cannot be passed to a
# function as arguments - these evaluate it inline and report y/n instead.
cond()  { if eval "[[ $1 ]]"; then echo y; else echo n; fi; }

# The C locale keeps printf's decimal point, sorting and globbing
# deterministic (LikeOS has only the C locale; a host run should match).
export LC_ALL=C

TMP=/tmp/testbash.$$
mkdir -p "$TMP" || { echo "cannot create $TMP"; exit 1; }
trap 'rm -rf "$TMP"' EXIT

printf '========================================\n'
printf '  bash script test  (%s)\n' "$BASH_VERSION"
printf '========================================\n'

# ---------------------------------------------------------------------------
section "Parameters and quoting"
# ---------------------------------------------------------------------------
v=hello
ok "variable expansion"          "$v"                 "hello"
ok "braced expansion"            "${v}world"          "helloworld"
ok "single quotes are literal"   '$v'                 '$v'
ok "double quotes expand"        "$v there"           "hello there"
ok "backslash escape"            "\$v"                '$v'
ok "default value"               "${undef:-fallback}" "fallback"
ok "alternate value"             "${v:+set}"          "set"
ok "string length"               "${#v}"              "5"
ok "substring"                   "${v:1:3}"           "ell"
ok "remove prefix"               "${v#he}"            "llo"
ok "remove suffix"               "${v%lo}"            "hel"
ok "pattern substitution"        "${v/l/L}"           "heLlo"
ok "global substitution"         "${v//l/L}"          "heLLo"
ok "uppercase conversion"        "${v^^}"             "HELLO"
ok "positional \$0 is the script" "${0##*/}"          "testbash.sh"
set -- one two three
ok "positional parameters"       "$1-$2-$3"           "one-two-three"
ok "parameter count"             "$#"                 "3"
ok "\$* joins with IFS"          "$*"                 "one two three"
shift 2
ok "shift"                       "$1"                 "three"
set --

# ---------------------------------------------------------------------------
section "Command substitution and arithmetic"
# ---------------------------------------------------------------------------
ok "command substitution"        "$(echo sub)"        "sub"
ok "backtick substitution"       "`echo old`"         "old"
ok "nested substitution"         "$(echo $(echo in))" "in"
ok "arithmetic expansion"        "$((2 + 3 * 4))"     "14"
ok "arithmetic precedence"       "$(( (2 + 3) * 4 ))" "20"
ok "arithmetic variables"        "$((5 % 3))"         "2"
ok "arithmetic comparison"       "$((3 > 2))"         "1"
ok "bitwise ops"                 "$((6 & 3))-$((6 | 3))-$((6 ^ 3))" "2-7-5"
ok "shifts"                      "$((1 << 4))"        "16"
n=7
ok "arithmetic on variable"      "$((n * n))"         "49"
((n++))
ok "((n++)) increments"          "$n"                 "8"
ok "ternary"                     "$((n > 5 ? 1 : 0))" "1"
yes_ "(( )) true is success"     test "$((1 < 2))" = 1
let "m = 3 + 4"
ok "let builtin"                 "$m"                 "7"

# ---------------------------------------------------------------------------
section "Conditionals"
# ---------------------------------------------------------------------------
yes_ "[ string equality ]"       [ abc = abc ]
no_  "[ string inequality ]"     [ abc = abd ]
yes_ "[ -n nonempty ]"           [ -n "x" ]
yes_ "[ -z empty ]"              [ -z "" ]
yes_ "[ numeric -eq ]"           [ 5 -eq 5 ]
yes_ "[ numeric -lt ]"           [ 4 -lt 5 ]
ok "[[ pattern match ]]"         "$(cond 'foobar == foo*')"      "y"
ok "[[ pattern mismatch ]]"      "$(cond 'foobar == baz*')"      "n"
ok "[[ regex match ]]"           "$(cond 'foo123 =~ ^foo[0-9]+$')" "y"
ok "[[ regex mismatch ]]"        "$(cond 'foo =~ ^[0-9]+$')"     "n"
ok "[[ -f on a real file ]]"     "$(cond '-f /etc/passwd')"      "y"
ok "[[ && inside brackets ]]"    "$(cond '1 == 1 && 2 == 2')"    "y"
if [[ 2023-07 =~ ^([0-9]{4})-([0-9]{2})$ ]]; then
	ok "BASH_REMATCH capture"    "${BASH_REMATCH[1]}/${BASH_REMATCH[2]}" "2023/07"
else
	ok "BASH_REMATCH capture"    "no-match" "2023/07"
fi
yes_ "&& shortcircuit"           true
ok "|| fallback"                 "$(false || echo fell)" "fell"
ok "! negation"                  "$(! false && echo neg)" "neg"
if true; then r=then; else r=else; fi
ok "if/then/else"                "$r"                 "then"
case abc in
	a*) r=star ;;
	*)  r=other ;;
esac
ok "case statement"              "$r"                 "star"

# ---------------------------------------------------------------------------
section "Loops and functions"
# ---------------------------------------------------------------------------
acc=
for i in a b c; do acc="$acc$i"; done
ok "for-in loop"                 "$acc"               "abc"
acc=
for ((i = 0; i < 4; i++)); do acc="$acc$i"; done
ok "C-style for loop"            "$acc"               "0123"
acc=; i=0
while [ $i -lt 3 ]; do acc="$acc$i"; i=$((i + 1)); done
ok "while loop"                  "$acc"               "012"
acc=; i=0
until [ $i -ge 3 ]; do acc="$acc$i"; i=$((i + 1)); done
ok "until loop"                  "$acc"               "012"
acc=
for i in 1 2 3 4 5; do
	[ $i -eq 2 ] && continue
	[ $i -eq 4 ] && break
	acc="$acc$i"
done
ok "break and continue"          "$acc"               "13"
acc=
for i in {1..5}; do acc="$acc$i"; done
ok "brace range expansion"       "$acc"               "12345"
ok "brace list expansion"        "$(echo x{a,b,c}y)"  "xay xby xcy"

greet() { echo "hi $1"; }
ok "function call"               "$(greet world)"     "hi world"
addup() { echo $(( $1 + $2 )); }
ok "function arguments"          "$(addup 20 22)"     "42"
retcode() { return 3; }
retcode
ok "function return status"      "$?"                 "3"
outer() { local scoped=inner; echo $scoped; }
scoped=outerval
outer > /dev/null
ok "local does not leak"         "$scoped"            "outerval"
recurse() { if [ $1 -le 0 ]; then echo done; else recurse $(( $1 - 1 )); fi; }
ok "recursive function"          "$(recurse 5)"       "done"

# ---------------------------------------------------------------------------
section "Arrays"
# ---------------------------------------------------------------------------
arr=(alpha beta gamma)
ok "array element"               "${arr[1]}"          "beta"
ok "array all elements"          "${arr[*]}"          "alpha beta gamma"
ok "array length"                "${#arr[@]}"         "3"
arr[3]=delta
ok "array append by index"       "${arr[3]}"          "delta"
arr+=(epsilon)
ok "array += append"             "${arr[4]}"          "epsilon"
ok "array slice"                 "${arr[*]:1:2}"      "beta gamma"
ok "array indices"               "${!arr[*]}"         "0 1 2 3 4"
unset 'arr[0]'
ok "unset array element"         "${#arr[@]}"         "4"
declare -A amap 2>/dev/null
if [ $? -eq 0 ]; then
	amap[key]=value
	amap[other]=thing
	ok "associative array"       "${amap[key]}"       "value"
	ok "associative array count" "${#amap[@]}"        "2"
else
	ok "associative array"       "unsupported"        "value"
fi
words=(one two three)
ok "iterate array"               "$(for w in "${words[@]}"; do printf '%s.' "$w"; done)" "one.two.three."

# ---------------------------------------------------------------------------
section "Redirection and pipelines"
# ---------------------------------------------------------------------------
echo "written" > "$TMP/a"
ok "write redirect"              "$(cat "$TMP/a")"    "written"
echo "appended" >> "$TMP/a"
ok "append redirect"             "$(wc -l < "$TMP/a")" "2"
ok "read redirect"               "$(cat < "$TMP/a" | head -1)" "written"
ok "pipeline"                    "$(echo piped | cat)" "piped"
ok "multi-stage pipeline"        "$(printf 'c\na\nb\n' | sort | head -1)" "a"
ok "stderr redirect"             "$( (echo err >&2) 2>&1 )" "err"
ok "stderr to null"              "$( (echo err >&2) 2>/dev/null )" ""
ok "here-string"                 "$(cat <<< 'from herestring')" "from herestring"
ok "here-document"               "$(cat <<EOF
heredoc body
EOF
)" "heredoc body"
ok "quoted heredoc is literal"   "$(cat <<'EOF'
$notexpanded
EOF
)" '$notexpanded'
ok "fd duplication"              "$(echo dup 3>&1 >&3 3>&-)" "dup"
{ echo grouped; } > "$TMP/b"
ok "brace group redirect"        "$(cat "$TMP/b")"    "grouped"
ok "subshell isolation"          "$( (x=inner; echo $x) )" "inner"
ok "pipeline exit status"        "$(false | true; echo $?)" "0"
ok "PIPESTATUS"                  "$(false | true; echo ${PIPESTATUS[0]})" "1"
ok "process substitution"        "$(cat <(echo procsub))" "procsub"
ok "write process substitution"  "$(echo ps-out > >(cat); sleep 1)" "ps-out"

# ---------------------------------------------------------------------------
section "Globbing"
# ---------------------------------------------------------------------------
touch "$TMP/f1.txt" "$TMP/f2.txt" "$TMP/other.log"
ok "star glob"                   "$(cd "$TMP" && echo *.txt)"    "f1.txt f2.txt"
ok "question glob"               "$(cd "$TMP" && echo f?.txt)"   "f1.txt f2.txt"
ok "bracket glob"                "$(cd "$TMP" && echo f[12].txt)" "f1.txt f2.txt"
ok "no-match stays literal"      "$(cd "$TMP" && echo *.nomatch)" "*.nomatch"
ok "glob in for loop"            "$(cd "$TMP" && for f in *.log; do echo $f; done)" "other.log"

# ---------------------------------------------------------------------------
section "Builtins"
# ---------------------------------------------------------------------------
ok "echo -n"                     "$(echo -n noline)"  "noline"
ok "echo -e escapes"             "$(echo -e 'a\tb' | tr '\t' '-')" "a-b"
ok "printf formatting"           "$(printf '%05.2f|%s|%d' 3.14159 str 42)" "03.14|str|42"
ok "printf -v"                   "$(printf -v pv 'x%sx' mid; echo $pv)" "xmidx"
ok "pwd is a directory"          "$(cd / && pwd)"     "/"
ok "cd and back"                 "$(cd /tmp && cd - > /dev/null && pwd)" "$(pwd)"
export EXPORTED=yes
ok "export visible to child"     "$(bash -c 'echo $EXPORTED')" "yes"
unset EXPORTED
ok "unset clears"                "${EXPORTED:-gone}"  "gone"
ok "read from pipe"              "$(echo data | { read line; echo $line; })" "data"
ok "read -r keeps backslash"     "$(printf 'a\\b\n' | { read -r l; echo "$l"; })" 'a\b'
ok "IFS-split read"              "$(echo 'a:b' | { IFS=: read x y; echo $y-$x; })" "b-a"
ok "eval"                        "$(cmd='echo evaled'; eval $cmd)" "evaled"
ok "type builtin"                "$(type -t cd)"      "builtin"
ok "type file"                   "$(type -t ls)"      "file"
ok "command -v"                  "$(command -v cd)"   "cd"
alias myalias='echo aliased'
shopt -s expand_aliases
ok "alias defined"               "$(alias myalias > /dev/null && echo yes)" "yes"
unalias myalias 2>/dev/null
ok "true/false status"           "$(true; echo $?)-$(false; echo $?)" "0-1"
ok "exit status of last cmd"     "$(bash -c 'exit 5'; echo $?)" "5"
ok "source a file"               "$(echo 'echo sourced' > "$TMP/s.sh"; . "$TMP/s.sh")" "sourced"
ok "getopts"                     "$(set -- -a -b val; while getopts 'ab:' o; do case $o in a) printf 'A';; b) printf 'B%s' "$OPTARG";; esac; done)" "ABval"
ok "declare -i integer"          "$(declare -i di; di=3+4; echo $di)" "7"
ok "readonly rejects write"      "$(readonly ro=1; (ro=2) 2>/dev/null; echo $ro)" "1"
ok "shopt query"                 "$(shopt -q expand_aliases && echo on)" "on"
ok "set -u errors on unset"      "$(bash -c 'set -u; echo ${nope}' 2>/dev/null || echo caught)" "caught"
ok "set -e exits on error"       "$(bash -c 'set -e; false; echo notreached' 2>/dev/null || echo caught)" "caught"
ok "set -o pipefail"             "$(bash -c 'set -o pipefail; false | true; echo $?')" "1"

# ---------------------------------------------------------------------------
section "Special variables"
# ---------------------------------------------------------------------------
yes_ "\$\$ is a pid"             [ "$$" -gt 0 ]
ok "\$? after true"              "$(true; echo $?)"   "0"
(exit 9); ok "\$? after subshell" "$?"                "9"
yes_ "HOME is set"               [ -n "$HOME" ]
yes_ "PATH is set"               [ -n "$PATH" ]
yes_ "BASH_VERSION is set"       [ -n "$BASH_VERSION" ]
ok "BASH_VERSINFO major"         "${BASH_VERSINFO[0]}" "5"
yes_ "SECONDS counts"            [ "$SECONDS" -ge 0 ]
yes_ "RANDOM in range"           [ "$RANDOM" -ge 0 -a "$RANDOM" -le 32767 ]
ok "LINENO is a number"          "$([ "$LINENO" -gt 0 ] && echo yes)" "yes"
ok "FUNCNAME inside function"    "$(f() { echo ${FUNCNAME[0]}; }; f)" "f"

# ---------------------------------------------------------------------------
section "Job control and processes"
# ---------------------------------------------------------------------------
sleep 2 &
bgpid=$!
yes_ "background job has a pid"  [ "$bgpid" -gt 0 ]
jobs > "$TMP/jobs1"   # not $( ): a subshell has its own empty job table
ok "jobs lists the job"          "$(grep -c Running "$TMP/jobs1")" "1"
kill "$bgpid" 2>/dev/null
wait "$bgpid" 2>/dev/null
jobs > "$TMP/jobs2"
ok "killed job is reaped"        "$(grep -c Running "$TMP/jobs2")" "0"
sleep 1 &
wait
ok "wait for all children"       "$?"                 "0"
ok "subshell pid differs"        "$( [ "$(bash -c 'echo $$')" != "$$" ] && echo differ )" "differ"
ok "exit status of signalled"    "$(bash -c 'kill -TERM $$' 2>/dev/null; echo $?)" "143"
ok "command in subshell"         "$(echo $( (echo nested) ))" "nested"

# ---------------------------------------------------------------------------
section "Traps and signals"
# ---------------------------------------------------------------------------
ok "trap EXIT runs"              "$(bash -c 'trap "echo bye" EXIT; true')" "bye"
ok "trap can be reset"           "$(bash -c 'trap "echo bye" EXIT; trap - EXIT; true')" ""
ok "trap USR1 handled"           "$(bash -c 'trap "echo got" USR1; kill -USR1 $$; sleep 0')" "got"
ok "trap lists handlers"         "$(bash -c 'trap "echo x" USR1; trap -p USR1 | grep -c USR1')" "1"

# ---------------------------------------------------------------------------
section "Script features"
# ---------------------------------------------------------------------------
cat > "$TMP/child.sh" <<'EOF'
#!/bin/bash
echo "child:$1"
exit 0
EOF
chmod 755 "$TMP/child.sh"
ok "shebang script executes"     "$("$TMP/child.sh" arg)" "child:arg"
ok "bash -c string"              "$(bash -c 'echo minus-c')" "minus-c"
ok "bash script file"            "$(bash "$TMP/child.sh" via)" "child:via"
ok "exit code propagates"        "$(bash -c 'exit 42'; echo $?)" "42"
printf 'line1\nline2\nline3\n' > "$TMP/lines"
count=0
while read -r line; do count=$((count + 1)); done < "$TMP/lines"
ok "read loop over file"         "$count"             "3"
mapfile -t marr < "$TMP/lines" 2>/dev/null
if [ $? -eq 0 ]; then
	ok "mapfile reads lines"     "${marr[1]}"         "line2"
else
	ok "mapfile reads lines"     "unsupported"        "line2"
fi
ok "\$(<file) reads content"     "$(<"$TMP/b")"       "grouped"

printf '\n========================================\n'
printf '  passed: %d   failed: %d\n' "$pass" "$fail"
printf '========================================\n'
exit $fail
