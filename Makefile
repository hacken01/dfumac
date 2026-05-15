CXXFLAGS := -std=c++14
LDFLAGS := -framework CoreFoundation -framework IOKit -lc++

OBJS := main.o

all: dfumac

dfumac: $(OBJS)
	cc -o $@ $(OBJS) $(LDFLAGS)
clean:
	rm -f $(OBJS) dfumac