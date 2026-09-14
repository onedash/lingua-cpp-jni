#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
classes="$root/build/jni-classes"
case "$(uname -s)" in
  Darwin) native="$root/build/cmake/liblingua_jni.dylib" ;;
  Linux) native="$root/build/cmake/liblingua_jni.so" ;;
  *) echo "Unsupported host" >&2; exit 1 ;;
esac

rm -rf "$classes"
mkdir -p "$classes"
find "$root/jni/src/main" -name '*.java' -print0 | xargs -0 javac -d "$classes"
javac -cp "$classes" -d "$classes" \
  "$root/jni/src/test/java/io/github/onedash/linguacpp/LanguageDetectorSmoke.java"
java -Dlingua.cpp.native.path="$native" -cp "$classes" \
  io.github.onedash.linguacpp.LanguageDetectorSmoke "$root/data/model.bin"
