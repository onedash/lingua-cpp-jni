package io.github.onedash.linguacpp;

import io.github.onedash.linguacpp.internal.NativeDetector;

import java.io.IOException;
import java.nio.file.Path;
import java.util.Objects;

/**
 * Thread-safe, all-language detector for long-lived high-throughput services.
 *
 * <p>The first call to {@link #load(Path)} loads one immutable native model for the process.
 * Calls with the same path are idempotent; a different path is rejected. Native detector
 * scratch space is created once per request thread and reused without a request-time lock.
 * The model intentionally remains loaded until process exit, so there is no close operation.
 */
public final class LanguageDetector {
    public static final int LANGUAGE_COUNT = Language.UNKNOWN.ordinal();
    private static final Language[] LANGUAGES = Language.values();

    private LanguageDetector() {}

    /** Loads the all-language model before the service starts accepting requests. */
    public static LanguageDetector load(Path modelPath) {
        Objects.requireNonNull(modelPath, "modelPath");
        final Path realPath;
        try {
            realPath = modelPath.toRealPath();
        } catch (IOException error) {
            throw new IllegalArgumentException("Cannot resolve language model " + modelPath, error);
        }
        if (NativeDetector.languageCount() != LANGUAGE_COUNT) {
            throw new LinkageError("Java and native language tables do not match");
        }
        NativeDetector.initialize(realPath.toString());
        return new LanguageDetector();
    }

    /** Uses {@code lingua.cpp.model.path}, or extracts the model bundled in the JAR. */
    public static LanguageDetector loadConfigured() {
        return load(NativeDetector.configuredModelPath());
    }

    /** Extracts and loads the model bundled in the JAR, ignoring model-path configuration. */
    public static LanguageDetector loadBundled() {
        return load(NativeDetector.bundledModelPath());
    }

    /** Fastest API: returns a stable {@link Language#ordinal()}, or {@code -1} if undecidable. */
    public int detectLanguageIndex(String text) {
        Objects.requireNonNull(text, "text");
        return NativeDetector.detect(text);
    }

    public Language detectLanguageOf(String text) {
        int index = detectLanguageIndex(text);
        return index < 0 ? Language.UNKNOWN : LANGUAGES[index];
    }

    /** Returns confidence values indexed by supported {@link Language#ordinal()}. */
    public double[] computeLanguageConfidenceValues(String text) {
        double[] output = new double[LANGUAGE_COUNT];
        fillLanguageConfidenceValues(text, output);
        return output;
    }

    /**
     * Allocation-free confidence API for callers that keep a 75-element array per thread.
     * Every element is overwritten.
     */
    public void fillLanguageConfidenceValues(String text, double[] output) {
        Objects.requireNonNull(text, "text");
        Objects.requireNonNull(output, "output");
        if (output.length != LANGUAGE_COUNT) {
            throw new IllegalArgumentException("output length must be " + LANGUAGE_COUNT);
        }
        NativeDetector.fillConfidenceValues(text, output);
    }

    public long modelMemoryBytes() {
        return NativeDetector.modelMemoryBytes();
    }
}
