plugins { id("com.android.application") }
android {
    namespace = "dev.kiraly.linuxaudio"
    compileSdk = 36
    defaultConfig {
        applicationId = "dev.kiraly.linuxaudio"
        minSdk = 35
        targetSdk = 35
        versionCode = 6
        versionName = "0.1.5"
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
