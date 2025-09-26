clang -std=c23 -o rolledit -O3 -I/opt/homebrew/include -L/opt/homebrew/lib -L/usr/local/lib -rpath /usr/local/lib -lSDL3 -lSDL3_TTF -lavformat -lavcodec -lswscale -lavutil -lswresample src/main.c
