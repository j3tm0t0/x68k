echo start
cat nosuch.txt 2>&1
echo "rc=$?"
grep -c apple in.txt
echo "after grep"
seq 5 | sort -rn | head -n 2
touch t.txt
echo "after touch"
mkdir -p d1/d2
pwd
env FOO=bar printenv FOO
ls nosuch 2>&1
echo "rc=$?"
find d1 -type d | sort
rm -r d1
ls -d d1 2>&1
printf "%s-%03d\n" x 7
date -u -d @0 +%Y
echo end
