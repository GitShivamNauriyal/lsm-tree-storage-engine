// skip_list.cpp — Explicit template instantiation for SkipList<string, string>.
// The template implementation lives in skip_list.h, but we instantiate here
// to produce a .o file that satisfies the Makefile dependency graph.

#include "skip_list.h"

namespace lsm {

// Explicit instantiation for the string-string variant used by MemTable.
template class SkipList<std::string, std::string>;

} // namespace lsm
