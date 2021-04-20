/*
 * NAME
 *   lddgraph - convert shared object dependencies into a directed graph
 *
 * SYNOPSIS
 *   lddgraph <dynamically-loadable-file> | dot -Tpng > g.png; eog g.png
 *   lddgraph <ldd-output-file> | dot -Tpng > g.png; eog g.png
 *   cat <ldd-output-file> | lddgraph - | dot -Tpng > g.png; eog g.png
 *
 * DECRIPTION
 *   examine the dynamically loaded executable or shared object file given as an
 *   argument, or the output of running ldd -v in a file argument or stdin (with
 *   - argument), and emit a graphviz directed graph DOT file on stdout.
 *
 *   The output DOT file may be passed to the 'dot' command to plot it into a
 *   displayable format.
 *
 *   Dashed lines are direct loader dependencies, at the top of the ldd -v
 *   listing - they show all the things ld.so will load along with the
 *   root executable or shared object.
 *
 *   Solid lines are the versioned symbol requirement dependencies.
 *
 * OPTIONS
 *   -    read ldd -v output on stdin 
 *   -?   provide help message
 *
 * EXAMPLES
 *   lddgraph /bin/bash | dot -Tpng > g.png && eog g.png
 *   lddgraph /usr/lib/libgdal.so | dot -Tpng > g.png; eog g.png
 *   ldd -v /bin/uname | lddgraph - | dot -Tpng > g.png; eog g.png
 *
 * BUGS
 *   Same issues as ldd has.
 *
 * SEE ALSO
 *   ld.so(8), ldconfig(8), ldd(8), dot(1), graphviz(7)
 *
 * COMPILE WITH
 *   c++ -std=c++98 -o lddgraph lddgraph.cpp
 *
 * AUTHOR
 *   James Perkins, 19 April 2021
 */

/*
 * Copyright 2021 James Perkins
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS 
 * IN THE SOFTWARE.
 */

// C++ APIs
#include <iostream>             // std::cin, cout, cerr, endl
#include <sstream>              // std::istringstream
#include <vector>               // std::vector

// C APIs
#include <errno.h>              // errno
#include <stdio.h>              // popen, pclose, FILE, BUFSIZ
#include <stdlib.h>             // exit, EXIT_SUCCESS, EXIT_FAILURE
#include <string.h>             // strerror
#include <unistd.h>             // access, X_OK

// uncomment for a lot of output to stderr
//#define DEBUG

#ifdef DEBUG
#define DEBUG_OUT(x) do { x; } while (0)
#else
#define DEBUG_OUT(x)
#endif

// remove "./" from front of string
static std::string trim_dot_slash(std::string s)
{
    unsigned int n = s.size();
    return (n > 1 && s[0] == '.' && s[1] == '/') ? s.substr(2) : s;
}

// remove ":" from end of string
static std::string trim_trailing_colon(std::string s)
{
    unsigned int n = s.size();
    return (n > 1 && s[n - 1] == ':') ? s.substr(0, n - 1) : s;
}

// remove "(" from start and ")" from end of string
static std::string trim_outer_parens(std::string s)
{
    unsigned int n = s.size();
    return (n > 1 && s[0] == '(' && s[n - 1] == ')') ? s.substr(1, n - 2) : s;
}

// Node represents a dynamically loadable shared object, executable or library
class Node
{
 private:
    std::string path;

 public:
    void dump(void)
    {
        DEBUG_OUT(std::cerr << "node: path " << path << std::endl);
    };

    Node(std::string p)
    {
        path = p;
        this->dump();
    };

    void setPath(std::string s)
    {
        path = s;
    };

    std::string getPath(void)
    {
        return path;
    };

    std::string getPathQuoted(void)
    {
        std::string s("\"");
        s += path;
        s += "\"";

        return s;
    };
};

// edge represents a single requirement between nodes and is labeled
// with the symbol version. If it is a versioned requirement it is
// marked as 'strong'. The non-strong requirements are marked with a
// dotted line and the strong ones with a solid line.
class Edge
{
 private:
    Node *from;
    Node *to;
    std::string label;
    bool strong;

 public:
    void dump(void)
    {
        DEBUG_OUT(std::cerr << "edge: from " << from->getPath() <<
            " to " << to->getPath() << " label " << label <<
            " strong " << strong << std::endl);
    }

    Edge(Node * f, Node * t, std::string l, bool s)
    {
        from = f;
        to = t;
        label = l;
        strong = s;
        this->dump();
    };

    Node *getFrom(void)
    {
        return from;
    };

    Node *getTo(void)
    {
        return to;
    };

    std::string getLabel(void)
    {
        return label;
    };

    bool getStrong(void)
    {
        return strong;
    };
};

// Process an input file, producing a directed graph description on output
void parse_file(const std::string patharg)
{
    std::string path = patharg;
    FILE *fp = NULL;
    bool is_pipe = false;

    // determine if input is stdin, executable or shared object, or regular
    // file input. executables and shared objects are run through ldd -v to
    // generate the input.
    if (path == "-")
    {
        fp = stdin;
    }
    else if (access(path.c_str(), X_OK) == 0 ||
        strstr(path.c_str(), ".so") != NULL)
    {
        // executable files and files with .so in the name
        std::string cmd("ldd -v ");

        cmd += path;

        fp = popen(cmd.c_str(), "r");
        is_pipe = true;
    }
    else if (access(path.c_str(), R_OK) == 0)
    {
        fp = fopen(path.c_str(), "r");
    }
    else
    {
        std::cerr << path.c_str() << ": " << strerror(errno) << std::endl;
    }

    if (fp == NULL)
    {
        const char *operation = is_pipe ? "popen" : "fopen";

        std::cerr << path << ": " << operation << " failed" << std::endl;
        exit(EXIT_FAILURE);
    }

    // Prepare a lists of nodes and edges
    std::vector < Node * >nodes;
    std::vector < Edge * >edges;

    // create a root node for the input file, also referred to as nodes[0]
    Node *cur_node = new Node(trim_dot_slash(path));
    nodes.push_back(cur_node);

    DEBUG_OUT(std::cerr << "node count " << nodes.size() << std::endl);

    // we're in the header of the ldd -v output until we see Version info:
    bool got_version_info = false;

    // read and process lines until EOF
    while (!feof(fp))
    {
        char line[BUFSIZ];

        // use janky old stdout fgets to grab a line
        if (fgets(line, sizeof(line), fp) == NULL)
        {
            break;
        }

        // convert into vector of strings, f
        std::vector < std::string > f;

        {
            std::string line_s = std::string(line);
            std::istringstream line_is(line_s);
            std::string fi;

            while (line_is >> fi)
            {
                f.push_back(fi);
            }
        }

        // input: not a <...>
        if (f.size() == 4 && f[0] == "not" && f[1] == "a")
        {
            std::cerr << path << ": not an dynamically loaded file" << std::endl;
            exit(EXIT_FAILURE);
        }

        // input: <path>: <libpath>: version `<symbol>' not found (required by <path>)
        if (f.size() == 5 && f[2] == "version" && f[4] == "not")
        {
            std::cerr << "some symbol versions are unresolvable, input: " << line;
            continue;
        }

        // input: Version information:
        if (f.size() == 2 && f[0] == "Version" && f[1] == "information:")
        {
            got_version_info = true;
            continue;
        }

        // input: <lib> (<loadaddr>)
        // input: <lib> => <path> (<loadaddr>)
        if (got_version_info == false && (f.size() == 2 || f.size() == 4))
        {
            std::string field(f.size() == 2 ? f[0] : f[2]);

            // input: <lib> => not found
            if (f.size() == 4 && f[2] == "not" && f[3] == "found")
            {
                std::cerr << f[0] << ": library not found, input: " << line;
                field = f[0];
            }

            Node *sub_node = new Node(trim_dot_slash(field));

            nodes.push_back(sub_node);

            DEBUG_OUT(std::cerr << "node count " << nodes.size() << std::endl);

            Edge *edge = new Edge(cur_node, sub_node, "", false);
            edges.push_back(edge);

            DEBUG_OUT(std::cerr << "edge count " << edges.size() << std::endl);

            continue;
        }

        if (got_version_info == false)
        {
            continue;
        }

        // input: <path>:
        if (f.size() == 1)
        {
            std::string field = trim_dot_slash(trim_trailing_colon(f[0]));

            if (path == "-")
            {
                nodes[0]->setPath(field);
                path = field;
                DEBUG_OUT(std::cerr << "reset path - to " << path << std::endl);
            }

            Node *sub_node = NULL;

            for (std::vector < Node * >::iterator pn = nodes.begin();
                pn != nodes.end(); ++pn)
            {
                if ((*pn)->getPath() == field)
                {
                    sub_node = *pn;
                    break;
                }
            }

            if (sub_node == NULL)
            {
                std::cerr << field << ": cannot find prior reference!" << std::endl;
                exit(EXIT_FAILURE);
            }

            cur_node = sub_node;

            continue;
        }

        // input: <library> (<version>) => <path>
        if (f.size() == 4)
        {
            std::string version(trim_outer_parens(f[1]));
            std::string field = trim_dot_slash(f[3]);
            Node *sub_node = NULL;

            for (std::vector < Node * >::iterator pn = nodes.begin();
                pn != nodes.end(); ++pn)
            {
                if ((*pn)->getPath() == field)
                {
                    sub_node = *pn;
                    break;
                }
            }

            if (sub_node == NULL)
            {
                std::cerr << field << ": cannot find prior reference!" <<
                    std::endl;
                exit(EXIT_FAILURE);
            }

            Edge *edge = new Edge(cur_node, sub_node, version, true);
            edges.push_back(edge);

            DEBUG_OUT(std::cerr << "edge count " << edges.size() << std::endl);

            continue;
        }

    }

    // if error, quit while we're behind
    if (!feof(fp))
    {
        std::cerr << path << ": aborted" << std::endl;
        exit(EXIT_FAILURE);
    }

    // close up input
    if (is_pipe && pclose(fp) != 0)
    {
        std::cerr << path << ": pclose:" << strerror(errno) << std::endl;
        exit(EXIT_FAILURE);
    }

    if (!is_pipe && fclose(fp) != 0)
    {
        std::cerr << path.c_str() << ": fclose:" << strerror(errno) << std::endl;
        exit(EXIT_FAILURE);
    }

    // Debug dump
    for (std::vector < Node * >::iterator pn = nodes.begin(); pn != nodes.end();
        ++pn)
    {
        (*pn)->dump();
    }

    for (std::vector < Edge * >::iterator pe = edges.begin(); pe != edges.end();
        ++pe)
    {
        (*pe)->dump();
    }

    // Begin output file on stdout
    std::cout << "digraph G {" << std::endl;

    // For each node, emit a digraph node
    for (std::vector < Node * >::iterator pn = nodes.begin(); pn != nodes.end();
        ++pn)
    {
        std::cout << (*pn)->getPathQuoted() << ";" << std::endl;
    }

     // Find all edges matching a node from and node to, and emit one digraph
     // edge that combines the labels into a single edge.
    for (std::vector < Node * >::iterator pf = nodes.begin(); pf != nodes.end();
        ++pf)
    {
        for (std::vector < Node * >::iterator pt = nodes.begin();
            pt != nodes.end(); ++pt)
        {
            // skip self-references (there won't be any, anyway)
            if (*pf == *pt)
            {
                continue;
            }

            // count matching edges, accumulate labels and strong (versioned) info
            unsigned int nedges = 0;
            bool strong = false;
            std::string labels("");

            for (std::vector < Edge * >::iterator pe = edges.begin();
                pe != edges.end(); ++pe)
            {
                if ((*pe)->getFrom() == *pf && (*pe)->getTo() == *pt)
                {
                    nedges++;
                    strong |= (*pe)->getStrong();
                    std::string label = (*pe)->getLabel();
                    if (label != "")
                    {
                        if (labels != "")
                        {
                            labels += "\\n";
                        }
                        labels += label;
                    }
                }
            }

            // no matching edges for this from and to?
            if (nedges == 0)
            {
                continue;
            }

            // emit the basic digraph edge
            std::cout << (*pf)->getPathQuoted() << " -> " <<
                (*pt)->getPathQuoted();

            // make the digraph edge dotted if no labels and not strong
            // and solid with label list of there are labels.
            if (labels == "")
            {
                if (!strong)
                {
                    std::cout << " [style=dotted]";
                }
            }
            else
            {
                std::cout << " [label=\"" << labels << "\"]";
            }

            std::cout << ";" << std::endl;
        }
    }

    // end digraph output
    std::cout << "}" << std::endl;
}

// main function
int main(int ac, char **av)
{
    // emit usage if:
    //  * no arguments
    //  * first argument has - followed by another letter
    if (ac < 2 || (ac > 1 && (av[1])[0] == '-' && (av[1])[1] != '\0'))
    {
        std::cerr <<
            "usage: lddgraph { - | ldd-output-file | dynamically-loadable-file }"
            << std::endl;
        exit(EXIT_FAILURE);
    }

    // iterate over input files
    while (--ac > 0)
    {
        parse_file(*++av);
    }

    exit(EXIT_SUCCESS);
}
