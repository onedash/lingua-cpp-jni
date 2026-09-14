#include "lingua/detector.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

int main(int argc, char** argv) try {
    using namespace lingua;
    if (argc < 2)
        throw std::runtime_error("Usage: lingua_tool compile RAW MODEL | selftest MODEL");

    const std::string mode = argv[1];
    if (mode == "compile") {
        if (argc != 4)
            throw std::runtime_error("Expected raw and model paths");
        Model::compile(argv[2], argv[3]);
        return 0;
    }
    if (mode != "selftest" || argc != 3)
        throw std::runtime_error("Usage: lingua_tool compile RAW MODEL | selftest MODEL");

    auto model = Model::load(argv[2]);
    if (model->memory_bytes() >= 300'000'000)
        throw std::runtime_error("Production model exceeds 300 MB");
    Detector detector(model);
    auto check = [&](std::string_view text, Language language) {
        if (detector.detect_language_of(text) != language)
            throw std::runtime_error("Regression failure: " + std::string(text));
    };
    check("", Language::Unknown);
    check("123 !!!", Language::Unknown);
    check("This is a sentence written in English.", Language::English);
    check("Dies ist ein deutscher Satz.", Language::German);
    check("Это предложение на русском языке.", Language::Russian);
    check("これは日本語です", Language::Japanese);
    for (auto text : {std::string("\xc0\x80", 2), std::string("\xed\xa0\x80", 3),
                      std::string("\xf4\x90\x80\x80", 4), std::string("\xe2", 1)}) {
        bool threw = false;
        try {
            detector.detect_language_of(text);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        if (!threw)
            throw std::runtime_error("Invalid UTF-8 accepted");
    }

    const std::array<std::string, 4> texts = {
        "hello world", "Я люблю читать книги", "日本語 and English", "école française"};
    std::array<std::array<double, language_count>, 4> expected;
    for (size_t i = 0; i < texts.size(); ++i)
        expected[i] = detector.compute_language_confidence_values(texts[i]);
    std::vector<std::thread> workers;
    std::array<bool, 8> passed{};
    for (size_t worker = 0; worker < passed.size(); ++worker)
        workers.emplace_back([&, worker] {
            Detector local(model);
            bool ok = true;
            for (int repeat = 0; repeat < 100; ++repeat)
                for (size_t i = 0; i < texts.size(); ++i)
                    ok &= local.compute_language_confidence_values(texts[i]) == expected[i];
            passed[worker] = ok;
        });
    for (auto& worker : workers)
        worker.join();
    for (bool ok : passed)
        if (!ok)
            throw std::runtime_error("Concurrent confidence mismatch");

    std::cout << "Selftest passed; model bytes=" << model->memory_bytes() << '\n';
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
