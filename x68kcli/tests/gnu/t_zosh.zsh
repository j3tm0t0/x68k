echo start
cat.x nosuch.txt 2>&1
echo "rc=$?"
grep -c apple in.txt
echo "after grep"
seq 5 | sort -rn | head -n 2
touch t.txt
echo "after touch"
mkdir -p d1/d2
pwd
env FOO=bar printenv FOO
ls.x nosuch 2>&1
echo "rc=$?"
find d1 -type d | sort
rm -r d1
ls.x -d d1 2>&1
printf "%s-%03d\n" x 7
date -u -d @0 +%Y
printf 'a\na\nb\n' > s1.txt
printf 'a\nb\n' > s3.txt
sdiff -s s1.txt s3.txt > /dev/null
echo "sdiff rc=$?"
sdiff -s s1.txt s1.txt > /dev/null
echo "sdiff same rc=$?"
export SHELL=A:/ZOSH.X
popen_t "printf '%s|%s\n' 'b c' d | tr '|' ':'"
popen_t false
unset SHELL
popen_t "echo x"
echo end
