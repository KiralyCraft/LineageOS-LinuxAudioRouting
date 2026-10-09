plugins { id("com.android.application") }
android {
    namespace = "dev.kiraly.linuxaudio"
    compileSdk = 36
    ndkVersion = "29.0.14206865"
    defaultConfig {
        applicationId = "dev.kiraly.linuxaudio"
        minSdk = 35
        targetSdk = 35
        versionCode = 7
        versionName = "0.1.6"
        ndk { abiFilters += "arm64-v8a" }
    }
    externalNativeBuild {
        cmake { path = file("../../native/CMakeLists.txt") }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
