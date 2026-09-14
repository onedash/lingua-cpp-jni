package io.github.onedash.linguacpp.internal;

import org.junit.jupiter.api.Test;

import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.HexFormat;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

class NativeLibraryLoaderTest {
    @Test
    void recognizesPublishedPlatforms() {
        assertEquals("windows-x86_64",
                NativeLibraryLoader.platform("Windows 11", "amd64", false));
        assertEquals("linux-x86_64",
                NativeLibraryLoader.platform("Linux", "x86_64", false));
        assertEquals("macos-aarch64",
                NativeLibraryLoader.platform("Mac OS X", "aarch64", false));
    }

    @Test
    void rejectsUnsupportedOrMuslPlatforms() {
        assertThrows(IllegalStateException.class,
                () -> NativeLibraryLoader.platform("Linux", "aarch64", false));
        var error = assertThrows(IllegalStateException.class,
                () -> NativeLibraryLoader.platform("Linux", "amd64", true));
        assertTrue(error.getMessage().contains("musl"));
    }

    @Test
    void mapsNativeFileNames() {
        assertEquals("lingua_jni.dll", NativeLibraryLoader.fileName("Windows 11"));
        assertEquals("liblingua_jni.so", NativeLibraryLoader.fileName("Linux"));
        assertEquals("liblingua_jni.dylib", NativeLibraryLoader.fileName("Darwin"));
    }

    @Test
    void bundledResourcesMatchTheirChecksums() throws Exception {
        String platform = NativeLibraryLoader.platform();
        checkResource("/native/" + platform + "/"
                + NativeLibraryLoader.fileName(System.getProperty("os.name")));
        checkResource("/model/model.bin");
    }

    private static void checkResource(String resource) throws Exception {
        byte[] buffer = new byte[1 << 16];
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (InputStream input = NativeLibraryLoaderTest.class.getResourceAsStream(resource)) {
            assertTrue(input != null, "missing " + resource);
            for (int read = input.read(buffer); read >= 0; read = input.read(buffer)) {
                digest.update(buffer, 0, read);
            }
        }
        String expected;
        try (InputStream input = NativeLibraryLoaderTest.class
                .getResourceAsStream(resource + ".sha256")) {
            assertTrue(input != null, "missing checksum for " + resource);
            expected = new String(input.readAllBytes(), StandardCharsets.US_ASCII).trim();
        }
        assertEquals(expected, HexFormat.of().formatHex(digest.digest()));
    }
}
