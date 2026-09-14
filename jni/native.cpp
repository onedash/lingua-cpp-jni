#include "lingua/detector.hpp"
#include <jni.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace {
struct Runtime {
    std::mutex mutex;
    std::string model_path;
    std::shared_ptr<const lingua::Model> model;
};

Runtime &runtime() {
    static Runtime value;
    return value;
}

std::shared_ptr<const lingua::Model> model_snapshot() {
    auto &state = runtime();
    std::lock_guard lock(state.mutex);
    if (!state.model)
        throw std::logic_error("language model is not initialized");
    return state.model;
}

struct Worker {
    explicit Worker(std::shared_ptr<const lingua::Model> model) : detector(std::move(model)) {}
    lingua::Detector detector;
    std::string utf8;
};

Worker &worker() {
    thread_local auto value = std::make_unique<Worker>(model_snapshot());
    return *value;
}

struct PendingJavaException {};

void throw_java(JNIEnv *env, const char *type, const char *message) {
    if (!env->ExceptionCheck())
        if (const jclass exception = env->FindClass(type))
            env->ThrowNew(exception, message);
}

template <class F, class T> T guarded(JNIEnv *env, T fallback, F action) {
    try {
        return action();
    } catch (const PendingJavaException &) {
    } catch (const std::bad_alloc &) {
        throw_java(env, "java/lang/OutOfMemoryError", "native allocation failed");
    } catch (const std::invalid_argument &error) {
        throw_java(env, "java/lang/IllegalArgumentException", error.what());
    } catch (const std::exception &error) {
        throw_java(env, "java/lang/IllegalStateException", error.what());
    } catch (...) {
        throw_java(env, "java/lang/IllegalStateException", "unknown native failure");
    }
    return fallback;
}

void append_utf8(std::string &result, uint32_t cp) {
    if (cp <= 0x7f) {
        result.push_back(char(cp));
    } else if (cp <= 0x7ff) {
        result.push_back(char(0xc0 | (cp >> 6)));
        result.push_back(char(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        result.push_back(char(0xe0 | (cp >> 12)));
        result.push_back(char(0x80 | ((cp >> 6) & 0x3f)));
        result.push_back(char(0x80 | (cp & 0x3f)));
    } else {
        result.push_back(char(0xf0 | (cp >> 18)));
        result.push_back(char(0x80 | ((cp >> 12) & 0x3f)));
        result.push_back(char(0x80 | ((cp >> 6) & 0x3f)));
        result.push_back(char(0x80 | (cp & 0x3f)));
    }
}

struct StringChars {
    JNIEnv *env;
    jstring string;
    const jchar *data;
    ~StringChars() {
        env->ReleaseStringChars(string, data);
    }
};

std::string &read_string(JNIEnv *env, jstring input, std::string &output,
                         const char *argument_name) {
    if (!input) {
        throw_java(env, "java/lang/NullPointerException", argument_name);
        throw PendingJavaException{};
    }
    const jsize length = env->GetStringLength(input);
    const jchar *chars = env->GetStringChars(input, nullptr);
    if (!chars)
        throw PendingJavaException{};
    const StringChars release{env, input, chars};

    output.clear();
    output.reserve(size_t(length));
    for (jsize i = 0; i < length; ++i) {
        uint32_t cp = chars[i];
        if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < length && chars[i + 1] >= 0xdc00 &&
            chars[i + 1] <= 0xdfff) {
            cp = 0x10000 + ((cp - 0xd800) << 10) + (chars[++i] - 0xdc00);
        } else if (cp >= 0xd800 && cp <= 0xdfff) {
            // Java Strings may contain isolated UTF-16 surrogates. Treat them like the
            // standard Java charset encoder instead of passing malformed UTF-8 to Lingua.
            cp = 0xfffd;
        }
        append_utf8(output, cp);
    }
    return output;
}

std::string &read_text(JNIEnv *env, jstring text, Worker &state) {
    return read_string(env, text, state.utf8, "text");
}
} // namespace

extern "C" JNIEXPORT void JNICALL
Java_io_github_onedash_linguacpp_internal_NativeDetector_initialize(JNIEnv *env, jclass,
                                                                    jstring path) {
    guarded(env, false, [&] {
        // Initialization is cold-path work. Holding this lock prevents two startup threads
        // from mapping separate 253 MiB copies of the same all-language model.
        std::string model_path;
        read_string(env, path, model_path, "modelPath");

        auto &state = runtime();
        std::lock_guard lock(state.mutex);
        if (state.model) {
            if (state.model_path != model_path)
                throw std::invalid_argument("a different language model is already initialized");
            return false;
        }
        state.model = lingua::Model::load(model_path);
        state.model_path = std::move(model_path);
        return true;
    });
}

extern "C" JNIEXPORT jint JNICALL
Java_io_github_onedash_linguacpp_internal_NativeDetector_languageCount(JNIEnv *, jclass) {
    return lingua::language_count;
}

extern "C" JNIEXPORT jlong JNICALL
Java_io_github_onedash_linguacpp_internal_NativeDetector_modelMemoryBytes(JNIEnv *env, jclass) {
    return guarded(env, jlong(0), [] {
        const auto model = model_snapshot();
        return jlong(model->memory_bytes());
    });
}

extern "C" JNIEXPORT jint JNICALL
Java_io_github_onedash_linguacpp_internal_NativeDetector_detect(JNIEnv *env, jclass, jstring text) {
    return guarded(env, jint(-1), [&] {
        auto &state = worker();
        const auto language = state.detector.detect_language_of(read_text(env, text, state));
        return jint(language);
    });
}

extern "C" JNIEXPORT void JNICALL
Java_io_github_onedash_linguacpp_internal_NativeDetector_fillConfidenceValues(JNIEnv *env, jclass,
                                                                              jstring text,
                                                                              jdoubleArray output) {
    guarded(env, false, [&] {
        if (!output) {
            throw_java(env, "java/lang/NullPointerException", "output");
            throw PendingJavaException{};
        }
        if (env->GetArrayLength(output) != lingua::language_count)
            throw std::invalid_argument("output must have one element per supported language");

        auto &state = worker();
        const auto values =
            state.detector.compute_language_confidence_values(read_text(env, text, state));
        env->SetDoubleArrayRegion(output, 0, lingua::language_count, values.data());
        if (env->ExceptionCheck())
            throw PendingJavaException{};
        return true;
    });
}
