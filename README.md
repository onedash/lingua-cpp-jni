# Lingua C++ JNI

[![Build](https://github.com/onedash/lingua-cpp-jni/actions/workflows/build.yml/badge.svg)](https://github.com/onedash/lingua-cpp-jni/actions/workflows/build.yml)

A C++20 language detector and Java 21 adapter based on the probability models
and rules from `lingua-rs`. It supports all 75 languages with one shared,
immutable model and has no Rust runtime dependency.

The API focuses on high-throughput services: language detection and confidence
values are supported; builders, unloading, low-accuracy mode, language subsets,
and mixed-language segmentation are not.

## Benchmark

**For a high-load service, `cpp` wins every metric by a wide margin.**

| | jvm 1.2.2 | cpp 1.9.0-2 | rs 1.8.0-2 |
|---|---:|---:|---:|
| `detectLanguageOf`, 1 thread, calls/s | 3,701 | **39,388** | 1,661 |
| `detectLanguageOf`, 8 threads, calls/s | 5,631 | **285,422** | 9,878 |
| p50 / p99 latency, 8 threads, ms | 0.80 / 14.4 | **0.014 / 0.19** | 0.39 / 4.8 |
| Model RAM | 1,453 MiB heap | 255 MiB native (reported) | ~300 MiB, mostly file-backed mmap |
| Process RSS while serving | 3,156 MiB | **364 MiB** | **344 MiB**  |
| Startup, fresh container / warm | 2.1 s / 2.1 s | 1.8 s / 0.41 s | 1.35 s / **0.24 s** |
| Raw accuracy on labeled corpus | 89.3% | **90.4%** | **90.4%** |


## Java artifact

Each release publishes one universal JAR:

```xml
<dependency>
  <groupId>io.github.onedash</groupId>
  <artifactId>lingua-cpp-jni</artifactId>
  <version>1.9.0-2</version>
</dependency>
```

It contains the Java API, the model, and native libraries for:

| Platform | Requirement |
| --- | --- |
| Windows x86-64 | Windows 10 / Server 2016 or newer; no VC Redistributable |
| Linux x86-64 | glibc 2.35 or newer; musl is not supported |
| macOS ARM64 | macOS 11 or newer |

The model is stored only once, so bundling three small native libraries adds
little to the JAR size.

Artifacts are published to GitHub Packages, which requires a classic personal
access token with `read:packages`, even for public repositories. Add this server
to `~/.m2/settings.xml`:

```xml
<server>
  <id>github-onedash</id>
  <username>YOUR_GITHUB_USERNAME</username>
  <password>${env.GITHUB_PACKAGES_TOKEN}</password>
</server>
```

Then add the repository:

```xml
<repository>
  <id>github-onedash</id>
  <url>https://maven.pkg.github.com/onedash/lingua-cpp-jni</url>
</repository>
```

The same JAR is attached to each GitHub Release. If GitHub Packages is not
suitable, download that asset and install it with
`maven-install-plugin:install-file`.

## Java usage

```java
var detector = LanguageDetector.loadBundled();
var language = detector.detectLanguageOf(text);

// Reuse this array per worker to avoid an allocation per request.
double[] confidence = new double[LanguageDetector.LANGUAGE_COUNT];
detector.fillLanguageConfidenceValues(text, confidence);
double english = confidence[Language.ENGLISH.ordinal()];
```

`LanguageDetector.loadBundled()` extracts and verifies the native library for
the current platform and the bundled model. `loadConfigured()` also honors the
explicit paths below. Resources are cached in checksum-addressed directories
under the system temporary directory, and atomic extraction is safe across
concurrent JVM startups.

| System property | Effect |
| --- | --- |
| `lingua.cpp.native.path` | Load a native library directly, useful for `noexec` temporary mounts |
| `lingua.cpp.model.path` | Load an external model instead of the bundled model |
| `lingua.cpp.cacheDir` | Set the extraction root; defaults to `java.io.tmpdir` |

The first load keeps one immutable native model for the process. Calls using the
same canonical model path share it; a different path is rejected. Every Java
thread lazily creates one native detector and reuses its scratch buffers without
a request-time lock. The model intentionally remains loaded until process exit.
A native library can belong to only one JVM classloader, so multi-deployment
application servers should place the JAR on a shared parent classloader.

Java strings are converted from UTF-16 while preserving NUL and supplementary
characters and safely replacing isolated surrogates. Undecidable input returns
`Language.UNKNOWN`; empty and non-letter inputs have zero confidence values.

## C++ usage

```cpp
#include <lingua/detector.hpp>

// Load once before accepting requests.
auto model = lingua::Model::load("data/model.bin");

// Create one detector per worker and share the model.
lingua::Detector detector(model);
auto language = detector.detect_language_of("languages are awesome");
auto confidence = detector.compute_language_confidence_values("hello world");
double english = confidence[static_cast<int>(lingua::Language::English)];
```

`Model` is immutable. Different detectors can run concurrently; one detector
allows one active call at a time because it reuses scratch buffers. Input is
UTF-8, and malformed UTF-8 throws `std::invalid_argument`.

## Confidence scales

Confidence values default to `Probability`: scores are exponentiated and
normalized to sum to 1 over the candidate languages. `Relative` matches the
original Lingua scale, where the winner is always `1.0` and other candidates
fall in `(0, 1]`. Both scales preserve the same ranking, but their distances and
thresholds are not interchangeable.

Choose the scale explicitly when relative values are needed; it cannot be
recovered from an already-normalized probability vector:

```cpp
auto relative = detector.compute_language_confidence_values(
    text, lingua::ConfidenceScale::Relative);
```

```java
double[] relative = detector.computeLanguageConfidenceValues(
        text, LanguageDetector.ConfidenceScale.RELATIVE);
```

## How detection works

The detector validates and decodes input, applies Unicode-aware lowercasing,
and splits it into script-appropriate words. Script and distinctive-character
rules can identify a language immediately or narrow the candidates.

The statistical stage extracts distinct character n-grams and looks each one up
once in an inverted index. Short text uses orders 1 through 5; text with at least
120 token characters uses trigrams. Missing n-grams fall back to shorter
prefixes. Candidate scores are accumulated and converted to the requested
confidence scale. A tie for the highest score produces `Language::Unknown`.

## Model and generated metadata

`generated/metadata.hpp` is checked-in runtime data. It defines the public
language enum and Unicode, script, and distinctive-character tables.
`data/model.bin` contains the n-gram probabilities. Regenerate both together
when changing the pinned `lingua-rs` checkout.

The compact model uses 267,582,938 bytes (255.19 MiB) in memory and its file is
265,371,148 bytes (253.08 MiB). A shared Unicode table adds 2.125 MiB;
per-detector scratch capacity grows with the largest input it has processed.
Load the model once and share it.

The model is a trusted, versioned build artifact, not a format for accepting
arbitrary models from service clients.

## Build and test

The runtime requires a little-endian C++20 platform with IEEE-754 doubles. To
build the current checked-in model and run native/JNI tests:

```powershell
cmake -S . -B build/cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake
ctest --test-dir build/cmake --output-on-failure
./tools/jni-smoke.ps1
```

Linux and macOS use `bash ./tools/jni-smoke.sh`. The Docker build reproduces the
published glibc Linux library, checks its dependencies and glibc symbol floor,
and tests the packaged JAR in a clean JRE:

```bash
docker build .
docker build --target native-artifact --output type=local,dest=dist .
```

The second command writes
`dist/native/linux-x86_64/liblingua_jni.so` for release assembly.

### Regenerate the model

Model generation additionally needs Rust and the adjacent `lingua-rs` checkout:

```powershell
./tools/prepare.ps1 -RustRoot ../lingua-rs
cargo build --release --offline --locked --manifest-path build/reference/Cargo.toml
./build/reference/target/release/lingua-reference.exe export ../lingua-rs data

cmake -S . -B build/cpp
cmake --build build/cpp --config Release
./build/cpp/Release/lingua_tool.exe compile data/ngrams.raw data/model.bin
ctest --test-dir build/cpp -C Release --output-on-failure
```

`data/ngrams.raw` is temporary; runtime needs only `data/model.bin`. Generation
has substantially higher peak memory than runtime because it sorts about 21.1
million exported records.

### Package the universal JAR

Build native libraries on their target operating systems and collect:

```text
jni/target/generated-resources/
├── model/model.bin
└── native/
    ├── windows-x86_64/lingua_jni.dll
    ├── linux-x86_64/liblingua_jni.so
    └── macos-aarch64/liblingua_jni.dylib
```

Do not run `mvn clean` after staging because `target` holds these inputs. Build
the single universal JAR with:

```bash
mvn -f jni/pom.xml -Pdist -DskipTests package
```

Ordinary `mvn -f jni/pom.xml verify` packages whichever local resources are
staged and runs loader tests. A platform CMake build plus `tools/jni-smoke.*`
tests the native call path directly.

## CI and releases

CI generates and verifies one model, builds the three native libraries on their
target platforms, and assembles one inspected universal JAR. Pull requests and
ordinary pushes build without publishing; manual runs expose the JAR as a
short-lived Actions artifact.

Tags publish to GitHub Packages and a GitHub Release:

```bash
git tag v1.9.0-x
git push origin v1.9.0-x
```

Versions use `<lingua-version>-<adapter-revision>`. GitHub Packages does not
allow overwriting a version, so use a new adapter revision instead of rerunning
an already-published tag. The built-in `GITHUB_TOKEN` supplies release and
package permissions.

## License

Apache-2.0. Models, metadata, rules, and scoring behavior derive from Peter M.
Stahl's Lingua project. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
