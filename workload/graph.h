// graph/graph.h

#pragma once
#include "node.h"
#include <unordered_map>

class Graph {
public:
    std::unordered_map<int, Node> nodes;

    void add_node(Node node);
    void add_edge(int src, int dst);

    std::vector<int> topological_sort();
};