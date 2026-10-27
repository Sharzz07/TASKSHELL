#!/bin/sh
# Real crash-recovery demo: SIGKILL the shell in the middle of a task, then resume.
rm -rf tasks logs state
printf 'task create longjob\ntask add longjob "echo step A done" --id a\ntask add longjob "sleep 6" --id b --after a\ntask add longjob "echo step C done" --id c --after b\ntask run longjob\n' > /tmp/crash_run1.txt
echo "=== Run 1: the shell is killed with SIGKILL (kill -9) after 3 seconds ==="
timeout -s KILL 3 ./taskshell < /tmp/crash_run1.txt
echo
echo "=== The shell is dead. Checkpoint left on disk: ==="
cat state/longjob.state
echo
echo "=== Run 2: a new shell resumes the task ==="
printf 'task resume longjob\nexit\n' | ./taskshell
