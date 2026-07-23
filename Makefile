CXXFLAGS = -O2 -shared -static -static-libgcc -static-libstdc++ -Wall -Wextra

all: build/GranularSampler_x64.dll build/GranularSampler_x86.dll

build/GranularSampler_x64.dll: src/plugin.cpp src/vst2.h
	mkdir -p build
	x86_64-w64-mingw32-g++ $(CXXFLAGS) -o $@ src/plugin.cpp

build/GranularSampler_x86.dll: src/plugin.cpp src/vst2.h
	mkdir -p build
	i686-w64-mingw32-g++ $(CXXFLAGS) -o $@ src/plugin.cpp

test: build/host64.exe build/host32.exe

build/host64.exe: test/host.c
	mkdir -p build
	x86_64-w64-mingw32-gcc -O2 -static -o $@ test/host.c

build/host32.exe: test/host.c
	mkdir -p build
	i686-w64-mingw32-gcc -O2 -static -o $@ test/host.c

clean:
	rm -rf build
