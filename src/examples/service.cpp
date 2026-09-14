#include <lingua/detector.hpp>
#include <iostream>
#include <thread>

int main(int argc, char** argv) {
    if (argc != 2)
        return 1;
    auto model = lingua::Model::load(argv[1]);
    // Keep one Detector in each service worker. Share this same model pointer.
    std::jthread worker([model] {
        lingua::Detector detector(model);
        std::cout << lingua::name(detector.detect_language_of("languages are awesome")) << '\n';
    });
}
