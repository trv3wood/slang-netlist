#include "netlist/NetlistNode.hpp"

#include <atomic>
#include <cstddef>

std::atomic<size_t> slang::netlist::NetlistNode::nextID{1};

auto slang::netlist::toString(NodeKind kind) -> std::string_view {
  switch (kind) {
  case NodeKind::None:
    return "none";
  case NodeKind::Port:
    return "port";
  case NodeKind::Variable:
    return "variable";
  case NodeKind::Assignment:
    return "assignment";
  case NodeKind::Conditional:
    return "conditional";
  case NodeKind::Case:
    return "case";
  case NodeKind::Merge:
    return "merge";
  case NodeKind::State:
    return "state";
  case NodeKind::Constant:
    return "constant";
  }
  return "none";
}
