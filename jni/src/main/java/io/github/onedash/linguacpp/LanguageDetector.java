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
 *
 * <p>Confidence values come on one of two scales, see {@link ConfidenceScale}. The default is
 * {@link ConfidenceScale#PROBABILITY} and can be switched process-wide with
 * {@code -Dlingua.cpp.confidence.scale=relative}, so an application whose thresholds were tuned
 * against Lingua's own confidence values can keep calling {@link #computeLanguageConfidenceValues}
 * unchanged. Code that wants one specific scale regardless of configuration should name it, with
 * {@link #computeLanguageConfidenceValues(String, ConfidenceScale)}.
 */
public final class LanguageDetector {
    public static final int LANGUAGE_COUNT = Language.UNKNOWN.ordinal();
    private static final Language[] LANGUAGES = Language.values();

    /** How far apart the reported confidence values place the candidate languages. */
    public enum ConfidenceScale {
        /**
         * Probabilities over the candidate languages, summing to 1. The winner's value is the
         * model's confidence in it, so a clear but unremarkable sentence can score below 0.2.
         */
        PROBABILITY,
        /**
         * Lingua's original relative scale: the ratio of the best log score to each language's
         * log score, so the winner is always exactly 1.0 and every other candidate falls in
         * (0, 1] by how far behind it is. This is what {@code com.github.pemistahl:lingua}
         * returns, and it cannot be derived from {@link #PROBABILITY} values after the fact:
         * normalizing discards the absolute log-score offset the ratio depends on.
         */
        RELATIVE
    }

    /** Scale used by the calls that do not name one. Read once; set it before the first call. */
    private static final ConfidenceScale DEFAULT_SCALE = defaultScale();

    private LanguageDetector() {}

    private static ConfidenceScale defaultScale() {
        final String configured = System.getProperty("lingua.cpp.confidence.scale", "probability");
        switch (configured) {
            case "probability":
                return ConfidenceScale.PROBABILITY;
            case "relative":
                return ConfidenceScale.RELATIVE;
            default:
                throw new IllegalArgumentException(
                        "lingua.cpp.confidence.scale must be \"probability\" or \"relative\", not \""
                                + configured + "\"");
        }
    }

    /** The scale {@link #computeLanguageConfidenceValues(String)} reports on. */
    public static ConfidenceScale defaultConfidenceScale() {
        return DEFAULT_SCALE;
    }

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

    /**
     * Returns confidence values indexed by supported {@link Language#ordinal()}, on the scale
     * {@link #defaultConfidenceScale()} reports.
     */
    public double[] computeLanguageConfidenceValues(String text) {
        return computeLanguageConfidenceValues(text, DEFAULT_SCALE);
    }

    /** Returns confidence values on the requested scale, ignoring the configured default. */
    public double[] computeLanguageConfidenceValues(String text, ConfidenceScale scale) {
        double[] output = new double[LANGUAGE_COUNT];
        fillLanguageConfidenceValues(text, output, scale);
        return output;
    }

    /**
     * Allocation-free confidence API for callers that keep a 75-element array per thread.
     * Every element is overwritten.
     */
    public void fillLanguageConfidenceValues(String text, double[] output) {
        fillLanguageConfidenceValues(text, output, DEFAULT_SCALE);
    }

    /** Allocation-free confidence API on the requested scale. Every element is overwritten. */
    public void fillLanguageConfidenceValues(String text, double[] output, ConfidenceScale scale) {
        Objects.requireNonNull(text, "text");
        Objects.requireNonNull(output, "output");
        Objects.requireNonNull(scale, "scale");
        if (output.length != LANGUAGE_COUNT) {
            throw new IllegalArgumentException("output length must be " + LANGUAGE_COUNT);
        }
        if (scale == ConfidenceScale.RELATIVE) {
            NativeDetector.fillRelativeConfidenceValues(text, output);
        } else {
            NativeDetector.fillConfidenceValues(text, output);
        }
    }

    public long modelMemoryBytes() {
        return NativeDetector.modelMemoryBytes();
    }
}
