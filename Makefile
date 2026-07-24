CXXFLAGS = -O2 -shared -static -static-libgcc -static-libstdc++ -Wall -Wextra -std=c++14
# Win32 GUI editor: GDI, common dialogs, shell (drag-and-drop)
LDLIBS = -lgdi32 -lcomdlg32 -lole32 -lshell32 -luser32

HDRS = src/vst2.h src/engine.h src/wav.h src/fft.h src/editor.h

all: build/GranularSampler_x64.dll build/GranularSampler_x86.dll

build/GranularSampler_x64.dll: src/plugin.cpp $(HDRS)
	mkdir -p build
	x86_64-w64-mingw32-g++ $(CXXFLAGS) -o $@ src/plugin.cpp $(LDLIBS)

build/GranularSampler_x86.dll: src/plugin.cpp $(HDRS)
	mkdir -p build
	i686-w64-mingw32-g++ $(CXXFLAGS) -o $@ src/plugin.cpp $(LDLIBS)

test: build/host64.exe build/host32.exe

build/host64.exe: test/host.c
	mkdir -p build
	x86_64-w64-mingw32-gcc -O2 -static -o $@ test/host.c

build/host32.exe: test/host.c
	mkdir -p build
	i686-w64-mingw32-gcc -O2 -static -o $@ test/host.c

# Native DSP unit test (host compiler): exercises the real audio path.
enginetest: build/engine_test
	./build/engine_test

build/engine_test: test/engine_test.cpp src/engine.h src/wav.h src/fft.h
	mkdir -p build
	$(CXX) -O2 -std=c++14 -Wall -o $@ test/engine_test.cpp

clean:
	rm -rf build
