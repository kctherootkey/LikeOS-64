# /etc/bash.bashrc - system-wide settings for INTERACTIVE bash shells.
#
# bash reads this automatically for interactive non-login shells (the shell
# was built with SYS_BASHRC pointing here).  Login shells do NOT read it on
# their own, so /etc/profile sources it explicitly - that way both kinds of
# interactive shell end up configured identically.

# Nothing here applies to a non-interactive shell (scripts): bail out early.
case $- in
	*i*) ;;
	*) return ;;
esac

# ---------------------------------------------------------------------------
# Prompt:  user@host:directory$      ($ for ordinary users, # for root)
#   \u user   \h hostname   \w cwd ($HOME shown as ~)   \$ prompt character
# ---------------------------------------------------------------------------
PS1='\u@\h:\w\$ '

# Coloured variant - the console understands ANSI SGR, so this works as-is.
# Uncomment to use it instead of the plain prompt above (root in red, other
# users in green, the directory in blue).
#if [ "$(id -u)" -eq 0 ]; then
#	PS1='\[\033[1;31m\]\u@\h\[\033[0m\]:\[\033[1;34m\]\w\[\033[0m\]\$ '
#else
#	PS1='\[\033[1;32m\]\u@\h\[\033[0m\]:\[\033[1;34m\]\w\[\033[0m\]\$ '
#fi

# Continuation prompt for multi-line commands, and the `select` prompt.
PS2='> '
PS3='#? '

# ---------------------------------------------------------------------------
# Interactive conveniences
# ---------------------------------------------------------------------------
# Keep a reasonable amount of history, drop duplicates and bare whitespace,
# and append rather than overwrite so several shells do not clobber it.
HISTSIZE=1000
HISTFILESIZE=2000
HISTCONTROL=ignoreboth
shopt -s histappend

# Re-check the window size after each command so $LINES/$COLUMNS stay right.
shopt -s checkwinsize

# Correct minor typos in directory names given to cd.
shopt -s cdspell

# Colour: ls and grep only colourise when asked, and the previous shell had
# these aliases built in - without them the console output is monochrome.
# "auto" means colour only when the output is a terminal, so pipes and
# redirections still get clean text.
alias ls='ls --color=auto'
alias grep='grep --color=auto'
alias ll='ls -alF'
alias la='ls -A'
alias l='ls -CF'
