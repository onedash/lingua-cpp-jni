package io.github.onedash.linguacpp.internal;

import java.nio.file.Path;

public final class NativeDetector {
    static {
        NativeLibraryLoader.load();
    }

    private NativeDetector() {}

    public static native void initialize(String modelPath);
    public static native int languageCount();
    public static native long modelMemoryBytes();
    public static native int detect(String text);
    public static native void fillConfidenceValues(String text, double[] output);

    public static Path configuredModelPath() {
        return NativeLibraryLoader.modelPath(true);
    }

    public static Path bundledModelPath() {
        return NativeLibraryLoader.modelPath(false);
    }
}
