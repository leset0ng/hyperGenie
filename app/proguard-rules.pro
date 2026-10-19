# Keep module entry classes and hookers reachable: the framework instantiates them
# reflectively from META-INF/xposed/java_init.list.
-keep class ng.lst.hha.HookEntry { *; }
-keepclasseswithmembers class * implements io.github.libxposed.api.XposedModuleInterface$* {
    <methods>;
}
-keepclasseswithmembers,allowoptimization class * implements io.github.libxposed.api.XposedInterface$Hooker {
    <methods>;
}

# Never warn about the compileOnly framework artifacts.
-dontwarn io.github.libxposed.**
