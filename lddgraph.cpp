/*
 * NAME
 *   lddgraph - convert shared object dependencies into a directed graph
 *
 * SYNOPSIS
 *   lddgraph <dynamically-loadable-file> | dot -Tpng > g.png; eog g.png
 *   lddgraph <ldd-output-file> | dot -Tpng > g.png; eog g.png
 *   cat <ldd-output-file> | lddgraph - | dot -Tpng > g.png; eog g.png
 *
 * DESCRIPTION
 *   Examine the dynamically loaded executable or shared object file given as an
 *   argument, or the output of running ldd -v in a file argument or stdin (with
 *   - argument), and emit a graphviz directed graph DOT file on stdout.
 *
 *   The output DOT file may be passed to the 'dot' command to plot it into a
 *   displayable format (e.g. png).
 *
 *   ld.so (the Linux ELF loader) will load and relocate every node of the output
 *   graph -- all the objects listed at the top of the ldd -v output.
 *
 *   Solid lines are the versioned symbol requirement dependencies.
 *
 *   Dotted lines are direct loader dependencies that weren't pulled in by
 *   the more-explicit versioned symbol dependencies.
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
 *   James Perkins, April 2021
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
#include <elf.h>                // ELF constants and offsets

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

typedef std::vector < Node * >Nodes;

// edge represents a single requirement between nodes and is labeled
// with the symbol version. If it is a versioned requirement it is
// marked as 'strong'. The non-strong requirements are marked with a
// dotted line and the strong ones with a solid line.
class Edge
{
 private:
    Node * from;
    Node *to;
    std::vector < std::string > labels;

 public:
    std::string getLabels(std::string delimiter)
    {
        std::string out;

        for (std::vector < std::string >::iterator ln = labels.begin();
            ln != labels.end(); ++ln)
        {
            if (ln != labels.begin())
            {
                out += delimiter;
            }

            out += *ln;
        }

        return out;
    };

    void dump(void)
    {
        DEBUG_OUT(std::cerr << "edge: from " << from->getPath() <<
            " to " << to->getPath() << " labels " <<
            getLabels(" ") << std::endl);
    };

    Edge(Node * f, Node * t)
    {
        from = f;
        to = t;
        this->dump();
    };

    void addLabel(std::string l)
    {
        labels.push_back(l);
    };

    Node *getFrom(void)
    {
        return from;
    };

    Node *getTo(void)
    {
        return to;
    };

    bool isLabeled(void)
    {
        return getLabels(" ") != "";
    };
};

typedef std::vector < Edge * >Edges;

static bool is_ELF_file(std::string path)
{
    // Open the file to determine if it is ELF
    FILE *fp = fopen(path.c_str(), "rb");

    if (fp == NULL)
    {
        std::cerr << path << ": fopen failed: " << strerror(errno) << std::endl;
        exit(EXIT_FAILURE);
    }

    // the ELF Identification header is 16 bytes and is followed by
    // the type field (which indicates if it is a dynamic load object).
    unsigned char s[EI_NIDENT + sizeof(Elf64_Half)];

    size_t n = fread(s, 1, sizeof(s), fp);

    if (n == 0)
    {
        std::cerr << path << ": fread failed: " << strerror(errno) << std::endl;
        fclose(fp);
        exit(EXIT_FAILURE);
    }

    fclose(fp);

    // a short file is not ELF
    if (n < sizeof(s))
    {
        DEBUG_OUT(std::cerr << path << ": short read" << std::endl);

        return false;
    }

    // a file with the wrong MAGIC or field values is not ELF
    if (s[EI_MAG0] != ELFMAG0 || s[EI_MAG1] != ELFMAG1 ||
        s[EI_MAG2] != ELFMAG2 || s[EI_MAG3] != ELFMAG3 ||
        !(s[EI_CLASS] == ELFCLASS32 || s[EI_CLASS] == ELFCLASS64) ||
        !(s[EI_DATA] == ELFDATA2LSB || s[EI_DATA] == ELFDATA2MSB) ||
        s[EI_VERSION] != EV_CURRENT ||
        !((s[EI_DATA] == ELFDATA2LSB && s[EI_NIDENT] == ET_DYN &&
                s[EI_NIDENT + 1] == 0) ||
            (s[EI_DATA] == ELFDATA2MSB && s[EI_NIDENT] == 0 &&
                s[EI_NIDENT + 1] == ET_DYN)))
    {
        DEBUG_OUT(std::cerr << path <<
            ": not recognized as an ELF dynamic load input file" << std::endl);
        return false;
    }

    // describe the file for debug folks
    unsigned int width = s[EI_CLASS] == ELFCLASS32 ? 32 :
        s[EI_CLASS] == ELFCLASS64 ? 64 : 0;

    DEBUG_OUT(std::cerr << path << ": " << width << " bits, dynamic load file"
        << std::endl);

    return true;
}

class PathFile
{
 private:
    std::string path;           // file pathname
    FILE *fp;                   // open file pointer
    bool is_pipe;               // opened with popen
    bool real_path_pending;     // ldd needs to tell us the pathname
    Node *cur_node;             // node to be used as from in edges
    bool got_version_info;      // false until we see Version info:

    bool process_line(Nodes & nodes, Edges & edges)
    {
        char line[BUFSIZ];

        // use janky old stdout fgets to grab a line
        if (fgets(line, sizeof(line), fp) == NULL)
        {
            return false;
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
            std::cerr << path << ": not an dynamically loaded file" <<
                std::endl;
            exit(EXIT_FAILURE);
        }

        // input: <path>: <libpath>: version `<symbol>' not found (required by <path>)
        if (f.size() == 5 && f[2] == "version" && f[4] == "not")
        {
            std::cerr <<
                "some symbol versions are unresolvable, input: " << line;
            return true;
        }

        // input: Version information:
        if (f.size() == 2 && f[0] == "Version" && f[1] == "information:")
        {
            got_version_info = true;
            return true;
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

            Edge *edge = new Edge(cur_node, sub_node);
            edges.push_back(edge);

            return true;
        }

        if (got_version_info == false)
        {
            return true;
        }

        // input: <path>:
        if (f.size() == 1)
        {
            std::string field = trim_dot_slash(trim_trailing_colon(f[0]));

            // we've been waiting for the ldd output to tell us what the
            // real file's path is.
            if (real_path_pending)
            {
                DEBUG_OUT(std::cerr << "reset path " << path << " to " <<
                    field << std::endl);
                nodes[0]->setPath(field);
                path = field;
                real_path_pending = false;
            }

            Node *sub_node = NULL;

            for (Nodes::iterator pn = nodes.begin(); pn != nodes.end(); ++pn)
            {
                if ((*pn)->getPath() == field)
                {
                    sub_node = *pn;
                    break;
                }
            }

            if (sub_node == NULL)
            {
                std::cerr << field <<
                    ": cannot find prior reference!" << std::endl;
                exit(EXIT_FAILURE);
            }

            cur_node = sub_node;

            return true;
        }

        // input: <library> (<version>) => <path>
        if (f.size() != 4)
        {
            return true;
        }

        std::string version(trim_outer_parens(f[1]));
        std::string field = trim_dot_slash(f[3]);
        Node *sub_node = NULL;

        for (Nodes::iterator pn = nodes.begin(); pn != nodes.end(); ++pn)
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

        // add label to existing edge, or create new edge

        bool found = false;

        for (Edges::iterator pe = edges.begin(); pe != edges.end(); ++pe)
        {
            if ((*pe)->getFrom() == cur_node && (*pe)->getTo() == sub_node)
            {
                found = true;
                (*pe)->addLabel(version);
            }
        }

        if (found == false)
        {
            Edge *edge = new Edge(cur_node, sub_node);
            edge->addLabel(version);
            edges.push_back(edge);
        }

        return true;
    }

 public:
    PathFile(std::string p)
    {
        path = p;
        fp = NULL;
        is_pipe = false;
        real_path_pending = false;
        cur_node = NULL;
        got_version_info = false;
    };

    void open(void)
    {
        // determine if input is stdin, executable or shared object, or regular
        // file input. executables and shared objects are run through ldd -v to
        // generate the input.
        if (path == "-")
        {
            fp = stdin;
            real_path_pending = true;
            return;
        }

        if (is_ELF_file(path))
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
            real_path_pending = true;
        }

        if (fp == NULL)
        {
            const char *operation = is_pipe ? "popen" : "fopen";

            std::cerr << path << ": " << operation << ": " <<
                strerror(errno) << std::endl;
            exit(EXIT_FAILURE);
        }
    };

    void parse(Nodes & nodes, Edges & edges)
    {
        // create a root node for the input file, also referred to as nodes[0]
        cur_node = new Node(trim_dot_slash(path));
        nodes.push_back(cur_node);

        // read and process lines until EOF
        while (!feof(fp))
        {
            if (process_line(nodes, edges) == false)
            {
                break;
            }
        }
    };

    void close()
    {
        // if error, quit while we're behind
        if (!feof(fp))
        {
            std::cerr << path << ": aborted" << std::endl;
            exit(EXIT_FAILURE);
        }

        if (is_pipe && pclose(fp) != 0)
        {
            std::cerr << path << ": pclose:" << strerror(errno) << std::endl;
            exit(EXIT_FAILURE);
        }

        if (!is_pipe && fclose(fp) != 0)
        {
            std::cerr << path.c_str() << ": fclose: " << strerror(errno) <<
                std::endl;
            exit(EXIT_FAILURE);
        }
    };
};

void trim_unlabeled_edges(Edges & edges)
{
    DEBUG_OUT(std::cerr << "edge count " << edges.size() << std::endl);

    // erase unlabeled edges with a to node for which there is a labeled edge pointing to it
    Edges saved_edges;

    for (Edges::iterator pe = edges.begin(); pe != edges.end(); ++pe)
    {
        bool erase = false;

        if ((*pe)->isLabeled())
        {
            goto save_edge;
        }

        for (Edges::iterator pf = edges.begin(); pf != edges.end(); pf++)
        {
            if ((*pf)->isLabeled() && (*pf)->getTo() == (*pe)->getTo())
            {
                DEBUG_OUT(std::cerr << "removing ");
                DEBUG_OUT((*pe)->dump());
                DEBUG_OUT(std::cerr << " because ");
                DEBUG_OUT((*pf)->dump());

                erase = true;
                break;
            }
        }

 save_edge:
        if (erase == false)
        {
            saved_edges.push_back(*pe);
        }
    }

    edges = saved_edges;
    DEBUG_OUT(std::cerr << "edge count " << edges.size() << std::endl);
}

void print_output(std::string & path, Nodes & nodes, Edges & edges)
{
    // Begin output file on stdout
    std::cout << "digraph G {" << std::endl;
    std::cout << "info_block [shape=box, label=\"" << "file: " << path <<
        "\\n" << "nodes: " << nodes.size() << "\\n" << "edges: " <<
        edges.size() << "\"];" << std::endl;

    // For each node, emit a digraph node
    for (Nodes::iterator pn = nodes.begin(); pn != nodes.end(); ++pn)
    {
        std::cout << (*pn)->getPathQuoted() << ";" << std::endl;
    }

    // Emit all edges

    for (Edges::iterator pe = edges.begin(); pe != edges.end(); ++pe)
    {
        // emit the basic digraph edge
        std::cout << (*pe)->getFrom()->getPathQuoted() << " -> " <<
            (*pe)->getTo()->getPathQuoted();

        // make the digraph edge solid if labeled, and dotted if not.

        if ((*pe)->isLabeled())
        {
            std::string labels = (*pe)->getLabels("\\n");
            std::cout << " [label=\"" << labels << "\"]";
        }
        else
        {
            std::cout << " [style=dotted]";
        }

        std::cout << ";" << std::endl;
    }

    // constrain output location of info block
    if (edges.size() > 0)
    {
        std::cout << edges[0]->getTo()->getPathQuoted() <<
            " -> info_block [style=invis];" << std::endl;
    }

    // end digraph output
    std::cout << "}" << std::endl;
}

// Process an input file, producing a directed graph description on output
void parse_file(std::string path)
{
    PathFile pathfile(path);
    Nodes nodes;
    Edges edges;

    // read the input file, producing nodes and edges
    pathfile.open();
    pathfile.parse(nodes, edges);
    pathfile.close();

    // Debug dump
    for (Nodes::iterator pn = nodes.begin(); pn != nodes.end(); ++pn)
    {
        (*pn)->dump();
    }

    for (Edges::iterator pe = edges.begin(); pe != edges.end(); ++pe)
    {
        (*pe)->dump();
    }

    trim_unlabeled_edges(edges);

    print_output(path, nodes, edges);
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
