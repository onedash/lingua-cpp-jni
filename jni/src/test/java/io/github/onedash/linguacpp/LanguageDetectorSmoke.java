package io.github.onedash.linguacpp;

import java.nio.file.Path;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.Executors;

public final class LanguageDetectorSmoke {
    private LanguageDetectorSmoke() {}

    public static void main(String[] arguments) throws Exception {
        LanguageDetector detector = arguments.length == 0
                ? LanguageDetector.loadBundled()
                : LanguageDetector.load(Path.of(arguments[0]));
        check(detector.modelMemoryBytes() == 267_582_938L);
        check(detector.detectLanguageOf("This is an English sentence") == Language.ENGLISH);
        check(detector.detectLanguageOf("日本語のテスト文章です") == Language.JAPANESE);
        check(detector.detectLanguageOf("한국어 테스트 문장입니다") == Language.KOREAN);
        check(detector.detectLanguageOf("😀 123 !!!") == Language.UNKNOWN);
        check(detector.detectLanguageOf("hello\0world") == Language.ENGLISH);
        check(detector.detectLanguageOf("\uD800") == Language.UNKNOWN);

        double[] values = new double[LanguageDetector.LANGUAGE_COUNT];
        detector.fillLanguageConfidenceValues("Bonjour tout le monde", values);
        check(values[Language.FRENCH.ordinal()] > values[Language.ENGLISH.ordinal()]);
        Arrays.fill(values, Double.NaN);
        detector.fillLanguageConfidenceValues("😀 123 !!!", values);
        check(Arrays.stream(values).allMatch(value -> value == 0.0));

        // The relative scale pins the winner at 1.0 and keeps the same ranking and candidates.
        double[] probabilities = detector.computeLanguageConfidenceValues(
                "Bonjour tout le monde", LanguageDetector.ConfidenceScale.PROBABILITY);
        double[] relative = detector.computeLanguageConfidenceValues(
                "Bonjour tout le monde", LanguageDetector.ConfidenceScale.RELATIVE);
        check(relative[Language.FRENCH.ordinal()] == 1.0);
        check(Math.abs(Arrays.stream(probabilities).sum() - 1.0) < 1e-9);
        for (int i = 0; i < relative.length; i++) {
            check((probabilities[i] > 0.0) == (relative[i] > 0.0));
            check(relative[i] <= 1.0);
        }
        Arrays.fill(relative, Double.NaN);
        detector.fillLanguageConfidenceValues(
                "😀 123 !!!", relative, LanguageDetector.ConfidenceScale.RELATIVE);
        check(Arrays.stream(relative).allMatch(value -> value == 0.0));

        List<String> texts = List.of(
                "The quick brown fox jumps over the lazy dog",
                "Bonjour tout le monde, comment allez-vous?",
                "Der schnelle braune Fuchs springt uber den faulen Hund",
                "Esta frase comprueba el detector de idiomas",
                "日本語のテスト文章です",
                "한국어 테스트 문장입니다");
        List<Language> expected = texts.stream().map(detector::detectLanguageOf).toList();
        List<double[]> expectedScores = texts.stream()
                .map(detector::computeLanguageConfidenceValues)
                .toList();
        try (var pool = Executors.newFixedThreadPool(16)) {
            var calls = java.util.stream.IntStream.range(0, 8_000).mapToObj(i ->
                    (java.util.concurrent.Callable<Boolean>) () -> {
                        int index = i % texts.size();
                        double[] actual = new double[LanguageDetector.LANGUAGE_COUNT];
                        detector.fillLanguageConfidenceValues(texts.get(index), actual);
                        return detector.detectLanguageOf(texts.get(index)) == expected.get(index)
                                && Arrays.equals(actual, expectedScores.get(index));
                    })
                    .toList();
            for (var result : pool.invokeAll(calls)) {
                check(result.get());
            }
        }
        System.out.println("JNI smoke test passed; model bytes=" + detector.modelMemoryBytes());
    }

    private static void check(boolean condition) {
        if (!condition) {
            throw new AssertionError();
        }
    }
}
