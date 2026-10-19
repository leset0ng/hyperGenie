plugins {
    alias(libs.plugins.android.application)
    // AGP 9+ compiles Kotlin itself (built-in Kotlin), so the standalone
    // org.jetbrains.kotlin.android plugin must not be applied here.
}

android {
    namespace = "ng.lst.hha"
    compileSdk = 37
    ndkVersion = "30.0.14904198"

    defaultConfig {
        applicationId = "ng.lst.hha"
        minSdk = 29
        targetSdk = 37
        versionCode = 1
        versionName = "1.0.0"

        // The launcher is arm64-only and the native_init payload matches that ABI.
        ndk {
            abiFilters += "arm64-v8a"
        }

        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_static")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            // Keep the entry class name stable so META-INF/xposed/java_init.list stays valid.
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_21
        targetCompatibility = JavaVersion.VERSION_21
    }

    packaging {
        resources {
            // Make sure the libxposed entry descriptors reach the APK untouched.
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
        // libxposed maps META-INF/xposed/native_init.list to this library name.
        jniLibs {
            useLegacyPackaging = false
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)

    // Provided by the framework at runtime: never bundle these into the APK.
    compileOnly(libs.libxposed.api)

    // Only needed when the module app talks to the framework service
    // (scope management, hot reload, remote preferences).
    // compileOnly(libs.libxposed.service)
}
