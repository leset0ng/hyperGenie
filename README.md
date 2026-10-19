# HyperHomeAnimation

A libxposed (LSPosed, API 102) module skeleton written in Kotlin, targeting the
HyperOS home screen (`com.miui.home`).

## Layout

```
app/src/main/
├── AndroidManifest.xml            # label/description come from resources
├── kotlin/ng/lst/hha/
│   └── HookEntry.kt               # XposedModule entry (the only one)
├── resources/META-INF/xposed/
│   ├── java_init.list             # entry class FQN(s)
│   ├── module.prop                # API versions, staticScope, exceptionMode, autoHotReload
│   └── scope.list                 # target packages, one per line
└── res/values/strings.xml         # module name / description shown by the manager
```

The module app has **no Activity**: it only exposes metadata to the framework.
If you need a settings screen later, add one and keep `module.prop` untouched.

## Requirements

| Item        | Value                       |
| ----------- | --------------------------- |
| Xposed API  | 102 (`io.github.libxposed`) |
| minSdk      | 29                          |
| compileSdk  | 37                          |
| AGP / Gradle| 9.4.1 / 9.8.1               |

`io.github.libxposed:api` is `compileOnly` — it is provided by the framework at
runtime and must never be bundled. `io.github.libxposed:service` is only needed
when the app itself wants to drive the framework (scope editing, hot reload,
remote preferences); it is commented out in `app/build.gradle.kts`.

## Build

```sh
./gradlew :app:assembleDebug
# -> app/build/outputs/apk/debug/app-debug.apk
```

## Before you hook for real

1. `META-INF/xposed/module.prop` — `minApiVersion` / `targetApiVersion` /
   `staticScope` / `exceptionMode` / `autoHotReload`.
2. `META-INF/xposed/scope.list` — every package the module should be injected
   into. `system` is a virtual package for system_server.
3. `HookEntry.onPackageReady` — verify class and method names against the
   current HyperOS build (they are obfuscated and change between releases).

Keep exactly **one** entry class: hot reload only works for single-entry modules.
