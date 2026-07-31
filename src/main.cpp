#include "engine.h"

#include <iostream>
#include <string>

int main() {
    std::cout << "LSM-Tree Key-Value Storage Engine" << std::endl;
    std::cout << "Use the test suite (make test) or benchmarks (make benchmark) for validation." << std::endl;
    
    // Simple interactive CLI could be added here
    lsm::Engine::Config config;
    config.data_dir = "data";
    
    lsm::Engine engine(config);
    
    return 0;
}
