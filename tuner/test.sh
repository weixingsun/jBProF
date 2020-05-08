rm -rf test1
echo "compile"
g++ -std=c++17 -O3 test1.cc -o test1 -lgp -I/usr/local/include/eigen3 -I/usr/local/include/gp
echo "run"
./test1
