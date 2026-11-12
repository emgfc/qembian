plugins {
    id("com.android.application")
}

android {
    namespace = "org.qembian.phone"
    compileSdk = 36
    defaultConfig {
        applicationId = "org.qembian.phone"
        minSdk = 30
        targetSdk = 36
        versionCode = 1
        versionName = "0.1"
        ndk { abiFilters += "arm64-v8a" }
    }
    buildTypes {
        release { isMinifyEnabled = false; signingConfig = signingConfigs.getByName("debug") }
    }
    // the native part is built by android/native/build.sh (QEMU + our board): its output is picked up here
    sourceSets["main"].jniLibs.srcDir("../../build/android/jniLibs")
    packaging { jniLibs { useLegacyPackaging = true } }   // extracted: QEMU's library is large, mmap'ed once
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
