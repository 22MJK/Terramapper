// workload/node.h

#pragma once
#include <vector>

enum NodeType {
    COMPUTE,
    ALLREDUCE,
    ALLGATHER,
    SEND,
    RECV
};

class Node {

public:

    int id;
    NodeType type;

    size_t compute_flops;
    size_t tensor_bytes;

    std::vector<int> parents;
    std::vector<int> children;

};