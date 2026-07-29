#!/bin/sh
# LikeOS-64 shebang self-test.  Run as:  SCRIPTTEST_MARK=1 scripttest.sh one two
echo "scripttest: interpreted by /bin/sh, script=$0"
echo "scripttest: argc=$# arg1=$1 arg2=$2"
echo "scripttest: HOME=$HOME PATH=$PATH"
echo "scripttest: pid=$$"
# Environment passthrough: a variable exported by the caller survives execve
env | grep SCRIPTTEST_MARK
# Nested interpreter: a script whose interpreter is /bin/echo proves the
# kernel argv order [interp, optarg, scriptpath, args...]
printf '#!/bin/echo -n\n' > /tmp/scripttest_nested
chmod 755 /tmp/scripttest_nested
/tmp/scripttest_nested nested-args-ok
echo ""
rm /tmp/scripttest_nested
# Nested shell script: recursion + exit-status propagation
printf '#!/bin/sh\necho nested-sh-ok\nexit 7\n' > /tmp/scripttest_sub
chmod 755 /tmp/scripttest_sub
/tmp/scripttest_sub
echo "scripttest: nested exit status=$? (expect 7)"
rm /tmp/scripttest_sub
# /usr/bin/env shebang: interpreter located via env
printf '#!/usr/bin/env sh\necho env-shebang-ok\n' > /tmp/scripttest_env
chmod 755 /tmp/scripttest_env
/tmp/scripttest_env
rm /tmp/scripttest_env
echo "scripttest: PASS"
exit 0
