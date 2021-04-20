#DEBUG := -DDEBUG
CXXFLAGS := -Wall -Wextra -O2 -std=c++98 $(DEBUG)

SRCS := lddgraph.cpp
OBJS := $(SRCS:%.cpp=%.o)
EXE := lddgraph

$(EXE): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $<

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $<

.PHONY: clean
clean:
	$(RM) $(OBJS) lddgraph
