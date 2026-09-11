# NativeInitFire carrier dex

`NativeInitFire.java` is the source of a 676-byte helper DEX that Vector appends to **every module
ClassLoader** so that deferred `native_init` entrypoints run with the module ClassLoader as ART's
JNI `FindClass` context under the soLoader path.

## Why the carrier class is needed

ART's JNI `FindClass` picks the ClassLoader from the **declaring class of the currently executing
native method** (`jni_internal.cc`: `GetClassLoader`). Under the soLoader path Vector calls
`native_init` from its own framework frame (`NativeAPI.loadNativeLibraries`), so `FindClass` inside
a module's `native_init` would resolve the framework ClassLoader and never see the module's own Java
classes.

rawVector had the module ClassLoader on the stack because the module itself executed
`System.loadLibrary`. Vector reproduces that context by executing the deferred `native_init` inside a
native method whose declaring class belongs to the module ClassLoader:

1. the carrier dex is appended to the module ClassLoader's dex list,
2. `loadClass("org.matrix.vector.nativeload.NativeInitFire")` resolves it,
3. `RegisterNatives` binds `fireNativeInit()V` to Vector's `NativeInitTrampoline`,
4. `CallStaticVoidMethod` triggers it, so the pending `native_init` entrypoints execute while ART's
   current method is `NativeInitFire.fireNativeInit` (module ClassLoader),
5. `FindClass` inside `native_init` now resolves the module's Java classes.

## Constraints

- **Do not add this class to the Kotlin/Java source sets.** If the framework ClassLoader defined it,
  the module ClassLoader would resolve the class from its parent and the declaring class would no
  longer belong to the module ClassLoader, breaking the trick.
- Keep the package out of the daemon's DEX obfuscation signature table
  (`daemon/src/main/jni/obfuscation.cpp`: `Lorg/matrix/vector/nativebridge/`, `.../core/`,
  `.../service/`). Those prefixes are rewritten to random same-length names in the framework dex,
  which would desynchronize the `NativeInitFireDex.CLASS_NAME` constant from the carrier dex. The
  carrier dex bytes themselves are embedded as base64 and are never rewritten.
- `RegisterNatives` requires the method to be declared native ahead of time, and ART's JNI
  `DefineClass` is an unsupported stub — verified against the target platform (Android 12 /
  SDK 31, `android12-release` `jni_internal.cc`: `LOG(WARNING) << "JNI DefineClass is not
  supported"`) as well as the android11/13/14/15 branches. Even if it existed, `DefineClass`
  would define the class into the *caller's* ClassLoader (the framework's), not the module's —
  appending the prebuilt dex to the module ClassLoader's dex list is the only way to get the
  declaring class owned by the module ClassLoader. That is why the class is shipped as a
  prebuilt dex.

## Regenerating the dex

```bash
# d8 from the Android SDK build-tools; --min-api 26 matches the framework's minimum.
mkdir -p build/nativeload
$ANDROID_HOME/build-tools/<version>/d8 \
  --min-api 26 --output build/nativeload tools/nativeload/NativeInitFire.java

# Expected artifact: build/nativeload/classes.dex (676 bytes)
sha256sum build/nativeload/classes.dex
# a8b3e50418546e809be900cf8d1e85af46df307e6d630c85ad19d331128b5947
```

Then replace `DEX_BASE64` in
`xposed/src/main/kotlin/org/matrix/vector/impl/core/NativeInitFireDex.kt` with the base64 of that
file (no line wrapping, `Base64.NO_WRAP`) and verify the round trip:

```bash
python -c "
import re, base64, hashlib
src = open('xposed/src/main/kotlin/org/matrix/vector/impl/core/NativeInitFireDex.kt', encoding='utf-8').read()
b64 = re.search(r'private const val DEX_BASE64 =\s*\"([^\"]+)\"', src).group(1)
raw = base64.b64decode(b64)
print(len(raw), hashlib.sha256(raw).hexdigest())
# must print: 676 a8b3e50418546e809be900cf8d1e85af46df307e6d630c85ad19d331128b5947
"
```
