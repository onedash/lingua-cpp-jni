package io.github.onedash.linguacpp.internal;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.HexFormat;
import java.util.Locale;

/** Extracts verified native and model resources into a content-addressed cache. */
final class NativeLibraryLoader {
    static final String NATIVE_PATH_PROPERTY = "lingua.cpp.native.path";
    static final String MODEL_PATH_PROPERTY = "lingua.cpp.model.path";
    static final String CACHE_DIR_PROPERTY = "lingua.cpp.cacheDir";
    private static boolean loaded;
    private static Path bundledModel;

    private NativeLibraryLoader() {}

    static synchronized void load() {
        if (loaded) {
            return;
        }
        String path = System.getProperty(NATIVE_PATH_PROPERTY);
        Path library = path == null || path.isBlank()
                ? extract("/native/" + platform() + "/" + fileName(), platform())
                : Path.of(path).toAbsolutePath();
        System.load(library.toString());
        loaded = true;
    }

    static synchronized Path modelPath(boolean useConfiguredPath) {
        String path = useConfiguredPath ? System.getProperty(MODEL_PATH_PROPERTY) : null;
        if (path != null && !path.isBlank()) {
            return Path.of(path).toAbsolutePath();
        }
        if (bundledModel == null) {
            bundledModel = extract("/model/model.bin", "model");
        }
        return bundledModel;
    }

    private static Path extract(String resource, String kind) {
        byte[] expected = expectedDigest(resource);
        String version = kind + "-" + HexFormat.of().formatHex(expected, 0, 16);
        Path directory = cacheDirectory(version);
        Path target = directory.resolve(Path.of(resource).getFileName().toString());
        if (digestMatches(target, expected)) {
            return target;
        }

        Path staging;
        try {
            staging = Files.createTempFile(directory, target.getFileName() + ".", ".tmp");
        } catch (IOException error) {
            throw extractionFailure(resource, directory, error);
        }
        try (InputStream input = open(resource)) {
            byte[] actual = copyAndDigest(input, staging);
            if (!MessageDigest.isEqual(expected, actual)) {
                throw new IllegalStateException(resource + " does not match its checksum");
            }
            publish(staging, target, expected);
            return target;
        } catch (IOException error) {
            throw extractionFailure(resource, directory, error);
        } finally {
            deleteQuietly(staging);
        }
    }

    private static void publish(Path staging, Path target, byte[] expected) throws IOException {
        try {
            Files.move(staging, target, StandardCopyOption.REPLACE_EXISTING,
                    StandardCopyOption.ATOMIC_MOVE);
        } catch (IOException error) {
            if (!digestMatches(target, expected)) {
                throw error;
            }
        }
    }

    private static byte[] copyAndDigest(InputStream input, Path target) throws IOException {
        MessageDigest digest = sha256();
        byte[] buffer = new byte[1 << 16];
        try (OutputStream output = Files.newOutputStream(target)) {
            for (int read = input.read(buffer); read >= 0; read = input.read(buffer)) {
                digest.update(buffer, 0, read);
                output.write(buffer, 0, read);
            }
        }
        return digest.digest();
    }

    private static boolean digestMatches(Path file, byte[] expected) {
        if (!Files.isRegularFile(file)) {
            return false;
        }
        try (InputStream input = Files.newInputStream(file)) {
            MessageDigest digest = sha256();
            byte[] buffer = new byte[1 << 16];
            for (int read = input.read(buffer); read >= 0; read = input.read(buffer)) {
                digest.update(buffer, 0, read);
            }
            return MessageDigest.isEqual(expected, digest.digest());
        } catch (IOException error) {
            return false;
        }
    }

    private static byte[] expectedDigest(String resource) {
        String sidecar = resource + ".sha256";
        try (InputStream input = NativeLibraryLoader.class.getResourceAsStream(sidecar)) {
            if (input == null) {
                throw new IllegalStateException("This JAR does not bundle " + sidecar);
            }
            String value = new String(input.readAllBytes(), StandardCharsets.US_ASCII).trim();
            if (value.length() != 64) {
                throw new IllegalStateException("Invalid checksum in " + sidecar);
            }
            return HexFormat.of().parseHex(value);
        } catch (IOException | IllegalArgumentException error) {
            throw new IllegalStateException("Cannot read checksum " + sidecar, error);
        }
    }

    private static InputStream open(String resource) {
        InputStream input = NativeLibraryLoader.class.getResourceAsStream(resource);
        if (input == null) {
            throw new IllegalStateException("This JAR does not bundle " + resource
                    + "; use the matching platform JAR or an explicit path property");
        }
        return input;
    }

    private static Path cacheDirectory(String version) {
        String root = System.getProperty(CACHE_DIR_PROPERTY);
        if (root == null || root.isBlank()) {
            root = System.getProperty("java.io.tmpdir");
        }
        Path directory = Path.of(root).toAbsolutePath()
                .resolve("lingua-cpp-jni-" + sanitize(System.getProperty("user.name", "unknown")))
                .resolve(version);
        try {
            Files.createDirectories(directory);
        } catch (IOException error) {
            throw new IllegalStateException("Cannot create extraction cache " + directory, error);
        }
        if (!Files.isDirectory(directory, LinkOption.NOFOLLOW_LINKS)) {
            throw new IllegalStateException(directory + " is not a directory");
        }
        return directory;
    }

    private static MessageDigest sha256() {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException error) {
            throw new AssertionError(error);
        }
    }

    private static IllegalStateException extractionFailure(
            String resource, Path directory, IOException error) {
        return new IllegalStateException("Cannot extract " + resource + " to " + directory
                + "; set -D" + CACHE_DIR_PROPERTY + " or an explicit path property", error);
    }

    private static void deleteQuietly(Path file) {
        try {
            Files.deleteIfExists(file);
        } catch (IOException ignored) {
            // A unique staging file is harmless and is never loaded.
        }
    }

    private static String sanitize(String value) {
        String result = value.replaceAll("[^\\p{L}\\p{N}]", "_");
        return result.isEmpty() ? "unknown" : result;
    }

    static String platform() {
        return platform(System.getProperty("os.name"), System.getProperty("os.arch"), muslLinux());
    }

    static String platform(String osName, String architecture, boolean musl) {
        String os = osName.toLowerCase(Locale.ROOT);
        String arch = architecture.toLowerCase(Locale.ROOT);
        boolean x64 = arch.equals("amd64") || arch.equals("x86_64") || arch.equals("x64");
        boolean arm64 = arch.equals("aarch64") || arch.equals("arm64");
        if (os.startsWith("windows") && x64) {
            return "windows-x86_64";
        }
        if ((os.contains("mac") || os.contains("darwin")) && arm64) {
            return "macos-aarch64";
        }
        if (os.contains("linux") && x64) {
            if (musl) {
                throw new IllegalStateException("musl Linux is not supported; set -D"
                        + NATIVE_PATH_PROPERTY + " to a compatible custom build");
            }
            return "linux-x86_64";
        }
        throw new IllegalStateException("Unsupported platform: " + osName + " / " + architecture
                + "; set -D" + NATIVE_PATH_PROPERTY + " to a compatible custom build");
    }

    static String fileName(String osName) {
        String os = osName.toLowerCase(Locale.ROOT);
        if (os.startsWith("windows")) {
            return "lingua_jni.dll";
        }
        if (os.contains("mac") || os.contains("darwin")) {
            return "liblingua_jni.dylib";
        }
        return "liblingua_jni.so";
    }

    private static String fileName() {
        return fileName(System.getProperty("os.name"));
    }

    private static boolean muslLinux() {
        return Files.exists(Path.of("/lib/ld-musl-x86_64.so.1"));
    }
}
