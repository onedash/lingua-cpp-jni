# Lingua C++ JNI

[![Build](https://github.com/onedash/lingua-cpp-jni/actions/workflows/build.yml/badge.svg)](https://github.com/onedash/lingua-cpp-jni/actions/workflows/build.yml)

A C++20, all-language detector built from the probability models and rules in
`lingua-rs`. The scoring engine uses one inverted n-gram index shared by all
service workers. It has no Rust runtime dependency.

The implementation retains the original high-accuracy scoring behavior. It
supports 75 languages, single-language detection and confidence values. There
is no builder, unloading, low-accuracy mode, language subset selection or
mixed-language segmentation API.

## Java artifacts and supported platforms

Every release publishes four runtime JARs under
`io.github.onedash:lingua-cpp-jni`:

| Artifact | Classifier | Runtime |
| --- | --- | --- |
| Universal | none | All platforms below |
| Windows x86-64 | `windows-x86_64` | Windows 10 / Server 2016 or newer; no VC Redistributable |
| Linux x86-64 | `linux-x86_64` | glibc 2.35 or newer; musl is not supported |
| macOS ARM64 | `macos-aarch64` | macOS 11 or newer |

Each JAR contains the Java API, the 75-language model, and either one native
library or all three. The model appears only once in the universal JAR, so its
size is close to a platform JAR rather than three times larger. Production
containers should normally use the platform classifier.

### Maven installation

GitHub Packages requires a classic personal access token with `read:packages`,
including for public repositories. Add a matching server to `~/.m2/settings.xml`:

```xml
<server>
  <id>github-onedash</id>
  <username>YOUR_GITHUB_USERNAME</username>
  <password>${env.GITHUB_PACKAGES_TOKEN}</password>
</server>
```

Then add the repository and dependency:

```xml
<repository>
  <id>github-onedash</id>
  <url>https://maven.pkg.github.com/onedash/lingua-cpp-jni</url>
</repository>

<dependency>
  <groupId>io.github.onedash</groupId>
  <artifactId>lingua-cpp-jni</artifactId>
  <version>1.9.0-1</version>
  <!-- Omit this line for the universal JAR. -->
  <classifier>linux-x86_64</classifier>
</dependency>
```

The same four JARs are attached to each GitHub Release. A
release asset is not a Maven repository; download it and use
`maven-install-plugin:install-file` when GitHub Packages is unsuitable.

## Service integration

```cpp
#include <lingua/detector.hpp>

// Once, before accepting requests:
auto model = lingua::Model::load("data/model.bin");

// One instance per worker; all workers receive the SAME model pointer:
lingua::Detector detector(model);
auto language = detector.detect_language_of("languages are awesome");
auto confidence = detector.compute_language_confidence_values("hello world");
double english = confidence[static_cast<int>(lingua::Language::English)];
```

`Model` is immutable after loading. Different detectors may execute concurrently;
one detector allows one active call at a time because it reuses its scratch
buffers. Model loading belongs at startup, not inside each request or detector
construction. There is no internal worker pool or request-time model lock.

## Java / JNI service adapter

The `jni` directory contains a Java 21 adapter designed for an all-language web
service. It intentionally has no builder or unload operation: `LanguageDetector.load`
loads `model.bin` once for the lifetime of the process, and repeated loads of that
same canonical path share it. Every Java request thread lazily creates one native
`Detector` and reuses its scratch buffers. After that first request on a thread,
detection does not take a registry or model lock.

```java
// Self-contained release JAR: extract and verify its native library and model.
var detector = LanguageDetector.loadBundled();
var language = detector.detectLanguageOf(text);

// Reuse this array per application worker to avoid a Java allocation per request.
double[] confidence = new double[LanguageDetector.LANGUAGE_COUNT];
detector.fillLanguageConfidenceValues(text, confidence);
double english = confidence[Language.ENGLISH.ordinal()];
```

`loadConfigured()` honors explicit native/model path properties and otherwise
uses the bundled resources. The native adapter converts Java UTF-16 itself,
preserving NUL and supplementary characters and replacing isolated surrogates
safely.

On first use, resources are extracted once into checksum-addressed directories:

```text
<tmp>/lingua-cpp-jni-<user>/<kind>-<sha256-prefix>/...
```

Every cached file is checked against the SHA-256 sidecar in the JAR before it is
loaded. Atomic publication makes concurrent JVM startup and interrupted
extractions safe. Available system properties:

| Property | Effect |
| --- | --- |
| `lingua.cpp.native.path` | Load a native library directly; useful for `noexec` temp mounts |
| `lingua.cpp.model.path` | Load an external model instead of extracting the bundled copy |
| `lingua.cpp.cacheDir` | Extraction root; defaults to `java.io.tmpdir` |
| `lingua.cpp.confidence.scale` | `probability` (default) or `relative`; see [Confidence scales](#confidence-scales). An unknown value fails at class initialization |

A native library can belong to only one JVM classloader. Application servers
with multiple deployments should put the JAR on a shared parent classloader.

Build and exercise it on Windows with:

```powershell
cmake -S . -B build/cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake
ctest --test-dir build/cmake --output-on-failure
./tools/jni-smoke.ps1
```

Linux and macOS use `bash ./tools/jni-smoke.sh` after the same CMake build.

Input is UTF-8; malformed UTF-8 throws `std::invalid_argument`. An undecidable
input returns `Language::Unknown`. Confidence values are an array indexed by
`Language`, not a sorted list. Empty and non-letter inputs have zero confidences.

## Confidence scales

The same scores can be reported on two scales. They rank the languages
identically and agree on which languages are candidates at all; they differ only
in how the distance between them is expressed.

`ConfidenceScale::Probability` is the default. Scores are exponentiated and
normalized, so they are probabilities summing to 1 over the candidates and the
winner's value is the model's confidence in it — `0.177` for a clear but
unremarkable English sentence, `1.0` for unambiguous Japanese.

`ConfidenceScale::Relative` is the scale `lingua-rs` and
`com.github.pemistahl:lingua` report: the ratio of the best log score to each
language's log score. Log scores are negative, so the winner is always exactly
`1.0` and the others fall in `(0, 1]` by how far behind they are.

Use `Relative` when thresholds were tuned against Lingua's own confidence
values. A policy of the form "act on the model only when the top two are more
than `t` apart, otherwise consult other signals" means something entirely
different against probabilities: a two-candidate probability split of
`0.29 / 0.08` looks decisive at any small `t`, while the same text on the
relative scale is `1.00 / 0.91` and correctly defers.

**The conversion runs only this way.** `p_i = exp(s_i)/Z` discards the absolute
log-score offset that `s_best/s_i` depends on, and recovering `s_i` would require
`log Z`, which the probability vector does not carry. Dividing probabilities by
their maximum restores a `1.0` at the top but not the original spacing, so the
scale has to be chosen before scoring, not afterwards.

```cpp
auto relative = detector.compute_language_confidence_values(
    text, lingua::ConfidenceScale::Relative);
```

```java
double[] relative = detector.computeLanguageConfidenceValues(
        text, LanguageDetector.ConfidenceScale.RELATIVE);
```

Callers that cannot change code can switch the process-wide default for the
no-scale overloads with `-Dlingua.cpp.confidence.scale=relative`.
`detect_language_of` and `detectLanguageOf` are unaffected by either setting:
the ranking is the same on both scales.

## How detection works

Detection has two stages. First, the input is validated and decoded from UTF-8,
lowercased with Unicode-aware mappings, and split into script-appropriate words.
Fast rules then count scripts and distinctive characters. A decisive rule result
is returned immediately; otherwise these counts reduce the set of candidate
languages.

The statistical stage extracts distinct character n-grams from the words. Short
text uses orders 1 through 5; text with at least 120 token characters uses only
trigrams. Each n-gram is looked up once in an inverted index whose postings hold
the languages and their log probabilities. When an exact n-gram is absent for a
language, the detector removes characters from the end and tries shorter
prefixes. Scores are accumulated per candidate, adjusted by the available
unigram count, and then turned into confidence values on the requested scale —
exponentiated and normalized by default, see [Confidence scales](#confidence-scales).
If the two highest values tie, `detect_language_of` returns `Language::Unknown`.

## Generated metadata

`generated/metadata.hpp` is required runtime data, despite its generated name.
It defines the public `Language` enum, language count and names, plus the Unicode
ranges, lowercase mappings, script-to-language masks, and distinctive-character
rules used by `src/detector.cpp`. It is generated separately from `data/model.bin`:
the header contains classification metadata and the binary file contains n-gram
probabilities. Removing the header would require replacing both its public API
definitions and its Unicode/rule tables.

The current compact model occupies **267,582,938 bytes (255.19 MiB)** in memory,
plus a shared 2.125 MiB Unicode property table and per-detector scratch space.
Its file is 265,371,148 bytes (253.08 MiB). Scratch capacity tracks the largest
input processed by that detector. Load the model once and share it; calling
`Model::load` repeatedly creates separate copies.

## Build and generate models

The runtime requires a little-endian C++20 platform with IEEE-754 doubles.
The model generation/reference tools also need Rust, the adjacent Lingua
checkout and the dependency versions listed in `tools/prepare.ps1`.

In PowerShell, from this directory:

```powershell
./tools/prepare.ps1 -RustRoot ../lingua-rs
cargo build --release --offline --locked --manifest-path build/reference/Cargo.toml
./build/reference/target/release/lingua-reference.exe export ../lingua-rs data

cmake -S . -B build/cpp
cmake --build build/cpp --config Release
./build/cpp/Release/lingua_tool.exe compile data/ngrams.raw data/model.bin
ctest --test-dir build/cpp -C Release --output-on-failure
```

The Ninja route also avoids duplicate `Path`/`PATH` environment issues seen with
MSBuild in some shells. Other installations can use their normal CMake toolchain.

`generated/metadata.hpp` is checked in and regenerated by the exporter. It
contains language names, Unicode ranges, lowercase mappings and rule metadata.
`data/ngrams.raw` is a temporary conversion file; runtime only needs
`data/model.bin`. Model generation has substantially higher peak memory than
runtime because it sorts all 21.1 million exported records. Generated binary
models and build outputs are excluded from version control.

The model file is a trusted build artifact with a versioned header and checked
array bounds. It is not a general-purpose format for accepting arbitrary models
from service clients. Regenerate metadata and model together when changing the
Rust checkout.

## Docker build

The Docker build reproduces the published glibc Linux x86-64 library, runs the
C++ and JNI tests, checks dynamic dependencies and the glibc symbol floor, and
loads the packaged JAR in a clean JRE image. `data/model.bin` must exist first.

```bash
docker build .
docker build --target native-artifact --output type=local,dest=dist .
```

The second command writes
`dist/native/linux-x86_64/liblingua_jni.so`, which is the layout consumed by the
release workflow.

## Building release JARs

Build each native library on its own target OS and collect the outputs under:

```text
jni/target/generated-resources/
├── model/model.bin
└── native/
    ├── windows-x86_64/lingua_jni.dll
    ├── linux-x86_64/liblingua_jni.so
    └── macos-aarch64/liblingua_jni.dylib
```

Do not run `mvn clean` after collecting them because `target` is the staging
directory. Then package all four runtime variants:

```bash
mvn -f jni/pom.xml -Pdist -DskipTests package
```

The ordinary `mvn -f jni/pom.xml verify` command builds a JAR from whichever
staged platform resources are present and runs the Java loader tests. A current
platform CMake build plus `tools/jni-smoke.*` tests the native call path directly.

## CI and releasing

`.github/workflows/build.yml` deliberately keeps model generation separate from
native compilation. On a cache miss it checks out the reviewed Lingua 1.9.0
commit `74eed3045bec53ba4feda2ac1780283f76fa302e`, regenerates metadata and the
compact model, requires the metadata to match the committed header, and uploads
one verified model artifact. Windows, Linux/Docker and macOS builds consume that
same artifact. The final job assembles and inspects exactly four runtime JARs.

Ordinary pushes and pull requests build and test without publishing. A manual
workflow run also exposes the four JARs as a short-lived Actions artifact. To
publish GitHub Packages and a GitHub Release:

```bash
git tag v1.9.0-1
git push origin v1.9.0-1
```

The built-in `GITHUB_TOKEN` supplies release and package permissions; no custom
repository secret is required. Versions use `<lingua-version>-<adapter-revision>`.
GitHub Packages does not allow overwriting an existing version, so release a new
adapter revision instead of rerunning an already-published tag.

The current Windows classifier JAR is about 172 MiB. The model dominates this
size; native libraries are small, so the other platform and universal variants
should be similar. CI enforces that all four expected runtime JARs exist and
validates their contents and size.

## License

Apache-2.0. Models, generated language/rule metadata and the compatible rule
and scoring behavior derive from Peter M. Stahl's Lingua project. See
[LICENSE](LICENSE) and [NOTICE](NOTICE).
