# Reproduces the release Linux x86-64 build in a glibc environment.
#
#   docker build .
#   docker build --target native-artifact --output type=local,dest=dist .

FROM maven:3.9.16-eclipse-temurin-21 AS maven-tools

FROM eclipse-temurin:21-jdk-jammy AS build

COPY --from=maven-tools /usr/share/maven /usr/share/maven
ENV PATH="/usr/share/maven/bin:${PATH}"

RUN apt-get update \
    && apt-get install --yes --no-install-recommends build-essential cmake ninja-build binutils file \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
COPY CMakeLists.txt LICENSE NOTICE ./
COPY include ./include
COPY generated ./generated
COPY src/model.cpp src/detector.cpp ./src/
COPY src/tests/scoring.cpp ./src/tests/
COPY tools/main.cpp tools/jni-smoke.sh ./tools/
COPY jni/pom.xml ./jni/pom.xml
RUN mvn -B -ntp -f jni/pom.xml dependency:go-offline

COPY jni/src ./jni/src
COPY jni/native.cpp ./jni/native.cpp
COPY data/model.bin ./data/model.bin

RUN cmake -S . -B build/cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DLINGUA_BUILD_EXAMPLES=OFF \
    && cmake --build build/cmake \
    && ctest --test-dir build/cmake --output-on-failure
RUN bash tools/jni-smoke.sh

RUN mkdir -p jni/target/generated-resources/native/linux-x86_64 \
        jni/target/generated-resources/model \
    && cp build/cmake/liblingua_jni.so \
        jni/target/generated-resources/native/linux-x86_64/ \
    && cp data/model.bin jni/target/generated-resources/model/ \
    && mvn -B -ntp -f jni/pom.xml verify

# The published Linux library must only depend on glibc and its own loader.
RUN set -eu; \
    library=build/cmake/liblingua_jni.so; \
    file "$library"; \
    needed="$(objdump -p "$library" | awk '/NEEDED/ {print $2}' | sort)"; \
    echo "Runtime dependencies: $needed"; \
    for soname in $needed; do \
        case "$soname" in \
            ld-linux-x86-64.so.2|libc.so.6|libm.so.6|libdl.so.2|libpthread.so.0|librt.so.1) ;; \
            *) echo "Unexpected runtime dependency: $soname" >&2; exit 1 ;; \
        esac; \
    done; \
    glibc="$(objdump -T "$library" | grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/[.]$//' | sort -uV | tail -1)"; \
    echo "Highest glibc symbol: $glibc"; \
    [ "$(printf '%s\nGLIBC_2.35\n' "$glibc" | sort -uV | tail -1)" = GLIBC_2.35 ]

# Proves resource extraction and model loading in a clean JRE-only image.
FROM eclipse-temurin:21-jre-jammy AS runtime-test
COPY --from=build /workspace/jni/target/lingua-cpp-jni-*.jar /app/lingua-cpp-jni.jar
COPY --from=build /workspace/jni/target/test-classes /app/test-classes
RUN java -cp /app/lingua-cpp-jni.jar:/app/test-classes \
    io.github.onedash.linguacpp.LanguageDetectorSmoke

FROM scratch AS native-artifact
COPY --from=build /workspace/build/cmake/liblingua_jni.so /native/linux-x86_64/liblingua_jni.so

FROM runtime-test AS test
