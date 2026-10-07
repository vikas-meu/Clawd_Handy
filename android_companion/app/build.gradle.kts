plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.vikas.clawdhandy"
    compileSdk = 35
    buildToolsVersion = "36.0.0"

    defaultConfig {
        applicationId = "com.vikas.clawdhandy"
        minSdk = 26
        targetSdk = 35
        versionCode = 3
        versionName = "2.2"
        // Phones are ARM; leaving out the x86 emulator copies of ML Kit's native code halves the APK
        ndk { abiFilters += listOf("arm64-v8a", "armeabi-v7a") }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    testOptions {
        unitTests.isReturnDefaultValues = true
    }

    packaging {
        resources {
            // The Anthropic SDK and its dependencies ship duplicate license/metadata files
            excludes += setOf("META-INF/*.md", "META-INF/LICENSE*", "META-INF/NOTICE*", "META-INF/DEPENDENCIES", "META-INF/versions/**")
        }
    }
}

dependencies {
    implementation("com.anthropic:anthropic-java:2.68.0")
    implementation("com.google.mlkit:barcode-scanning:17.3.0")
    implementation("com.google.mlkit:face-detection:16.1.7")
    implementation("com.google.android.gms:play-services-tasks:18.2.0")
    implementation("org.jsoup:jsoup:1.18.3")
    testImplementation("junit:junit:4.13.2")
}
