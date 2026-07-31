// engine.cpp — Placeholder for Phase 4 (compaction test linkage).
// Full implementation in Phase 5.

#include "engine.h"

namespace lsm {

std::atomic<uint64_t> Engine::flush_counter_{0};

Engine::Engine(const Config&) {}
Engine::~Engine() {}
void Engine::put(const std::string&, const std::string&) {}
void Engine::remove(const std::string&) {}
std::optional<std::string> Engine::get(const std::string&) { return std::nullopt; }
void Engine::flushMemTable() {}
void Engine::recover() {}
void Engine::reloadSSTableReaders() {}
std::string Engine::generateSSTablePath() { return ""; }

} // namespace lsm
