INSTALL := /usr/bin/install
INSTALL_PROGRAM := $(INSTALL) -m 0755
prefix := /usr/local
exec_prefix := $(prefix)
bindir := $(exec_prefix)/bin

#DEBUG := -DDEBUG
CXXFLAGS := -Wall -Wextra -O2 -std=c++98 $(DEBUG)

SRCS := lddgraph.cpp
OBJS := $(SRCS:%.cpp=%.o)
EXE := lddgraph

.PHONY: all
all: $(EXE)

$(EXE): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $<

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $<

.PHONY: install
install: $(bindir)/$(EXE)

.PHONY: install-strip
	$(MAKE) INSTALL_PROGRAM='$(INSTALL_PROGRAM) -s' install

$(bindir)/$(EXE): $(EXE)
	$(INSTALL_PROGRAM) -t $(bindir) $<

.PHONY: uninstall
uninstall:
	$(RM) $(bindir)/$(EXE)

.PHONY: clean
clean:
	$(RM) $(OBJS) lddgraph
