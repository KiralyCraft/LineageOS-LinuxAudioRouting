plugins { id("com.android.application") }
android {
    namespace = "dev.kiraly.linuxaudio"
    compileSdk = 36
    defaultConfig {
        applicationId = "dev.kiraly.linuxaudio"
        minSdk = 35
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
