CXXFLAGS = -O2 -shared -static -static-libgcc -static-libstdc++ -Wall -Wextra -std=c++14
# Win32 GUI editors: GDI, common dialogs, shell (drag-and-drop, folder picker)
LDLIBS = -lgdi32 -lcomdlg32 -lole32 -lshell32 -luser32

HDRS = src/vst2.h src/engine.h src/wav.h src/fft.h src/editor.h
LIBHDRS = src/vst2.h src/wav.h src/fft.h librarian/src/librarian.h librarian/src/editor.h

all: build/GranularSampler_x64.dll build/GranularSampler_x86.dll \
     build/SampleLibrarian_x64.dll build/SampleLibrarian_x86.dll

# ---- Granular Sampler ----

build/GranularSampler_x64.dll: src/plugin.cpp $(HDRS)
	mkdir -p build
	x86_64-w64-mingw32-g++ $(CXXFLAGS) -o $@ src/plugin.cpp $(LDLIBS)

build/GranularSampler_x86.dll: src/plugin.cpp $(HDRS)
	mkdir -p build
	i686-w64-mingw32-g++ $(CXXFLAGS) -o $@ src/plugin.cpp $(LDLIBS)

# ---- Sample Librarian ----

build/SampleLibrarian_x64.dll: librarian/src/plugin.cpp $(LIBHDRS)
	mkdir -p build
	x86_64-w64-mingw32-g++ $(CXXFLAGS) -Isrc -o $@ librarian/src/plugin.cpp $(LDLIBS)

build/SampleLibrarian_x86.dll: librarian/src/plugin.cpp $(LIBHDRS)
	mkdir -p build
	i686-w64-mingw32-g++ $(CXXFLAGS) -Isrc -o $@ librarian/src/plugin.cpp $(LDLIBS)

# ---- CI smoke-test hosts ----

test: build/host64.exe build/host32.exe

build/host64.exe: test/host.c
	mkdir -p build
	x86_64-w64-mingw32-gcc -O2 -static -o $@ test/host.c

build/host32.exe: test/host.c
	mkdir -p build
	i686-w64-mingw32-gcc -O2 -static -o $@ test/host.c

# ---- native DSP unit tests (host compiler): exercise the real audio path ----

enginetest: build/engine_test
	./build/engine_test

build/engine_test: test/engine_test.cpp src/engine.h src/wav.h src/fft.h
	mkdir -p build
	$(CXX) -O2 -std=c++14 -Wall -o $@ test/engine_test.cpp

librariantest: build/librarian_test
	./build/librarian_test

build/librarian_test: librarian/test/librarian_test.cpp librarian/src/librarian.h src/wav.h src/fft.h
	mkdir -p build
	$(CXX) -O2 -std=c++14 -Wall -Isrc -o $@ librarian/test/librarian_test.cpp

clean:
	rm -rf build
