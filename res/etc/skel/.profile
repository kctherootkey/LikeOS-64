# ~/.profile - per-user startup for a login shell.
#
# /etc/profile has already run (it sources /etc/bash.bashrc for interactive
# shells, which is where the prompt lives).  Sourcing ~/.bashrc here too means
# a login shell and a plain "bash" behave the same.

if [ -n "$BASH_VERSION" ] && [ -r "$HOME/.bashrc" ]; then
	. "$HOME/.bashrc"
fi
