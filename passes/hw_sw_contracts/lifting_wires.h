#pragma once

#include <string>

struct SingleWireLifting{
    std::string inner_module;
    std::string outer_module;
    std::string instance_name;
    std::string inner_wire;
    std::string outer_wire;
    SingleWireLifting(std::string inner_module, std::string outer_module, std::string instance_name, std::string inner_wire, std::string outer_wire) : inner_module(inner_module), outer_module(outer_module), instance_name(instance_name), inner_wire(inner_wire), outer_wire(outer_wire) { }

    bool operator==(const SingleWireLifting &other) const {
    return inner_module == other.inner_module &&
           outer_module == other.outer_module &&
           instance_name == other.instance_name &&
           inner_wire == other.inner_wire &&
           outer_wire == other.outer_wire;
    }
};