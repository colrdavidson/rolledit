clang -o rolledit -g -O0 $(pkg-config --cflags --libs libavformat) $(pkg-config --cflags --libs libavcodec) main.c
