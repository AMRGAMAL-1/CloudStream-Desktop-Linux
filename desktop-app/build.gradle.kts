plugins {
    kotlin("jvm")
    id("org.jetbrains.compose") version "1.11.1"
    id("org.jetbrains.kotlin.plugin.compose") version "2.3.20"
    alias(libs.plugins.kotlin.serialization)
}

java {
    sourceCompatibility = JavaVersion.VERSION_21
    targetCompatibility = JavaVersion.VERSION_21
}

sourceSets {
    main {
        java.srcDirs("src/main/java")
    }
}

tasks.withType<org.jetbrains.kotlin.gradle.tasks.KotlinCompile> {
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_21)
        freeCompilerArgs.add("-Xannotation-default-target=param-property")
    }
}

val isLinuxHost = System.getProperty("os.name", "").contains("linux", ignoreCase = true)
val isWindowsHost = System.getProperty("os.name", "").contains("win", ignoreCase = true)
val requiredRuntimeModules = listOf(
    "java.base",
    "java.desktop",
    "java.instrument",
    "java.logging",
    "java.management",
    "java.naming",
    "java.net.http",
    "java.prefs",
    "java.scripting",
    "java.sql",
    "java.xml",
    "jdk.dynalink",
    "jdk.unsupported",
    "jdk.crypto.ec",
    "jdk.crypto.cryptoki",
    "jdk.management",
    "jdk.charsets",
    "jdk.zipfs",
    "java.compiler",
    "jdk.compiler",
    "jdk.localedata",
) + if (isWindowsHost) listOf("jdk.crypto.mscapi") else emptyList()
val linuxNativeBridge by tasks.registering(Exec::class) {
    group = "native"
    description = "Builds the Linux WebKitGTK/JNI player bridge."
    workingDir(project.file("src/main/cpp"))
    commandLine("bash", "build_jni.sh")
    environment("JAVA_HOME", System.getProperty("java.home"))
    onlyIf { isLinuxHost }
}

configurations.all {
    exclude(group = "org.slf4j", module = "slf4j-simple")
    // Override the library module's strict constraint — desktop-app is pure JVM
    // and needs a newer jackson version to handle Kotlin 2.x @Metadata in plugins.
    resolutionStrategy.force("com.fasterxml.jackson.module:jackson-module-kotlin:2.18.3")
    resolutionStrategy.force("com.fasterxml.jackson.core:jackson-databind:2.18.3")
    resolutionStrategy.force("com.fasterxml.jackson.core:jackson-core:2.18.3")
    resolutionStrategy.force("com.fasterxml.jackson.core:jackson-annotations:2.18.3")
}

dependencies {
    // CloudStream Library (KMP, JVM target)
    // Contains: MainAPI, extractors, metaproviders, WebViewResolver (JVM actual), etc.
    implementation(project(":library"))
    implementation(libs.kotlinx.serialization.json)

    // ASM Bytecode Scanner
    implementation("org.ow2.asm:asm:9.6")
    implementation("org.ow2.asm:asm-tree:9.6")

    // Android Stubs
    implementation(project(":android-stubs"))

    implementation(project(":plugin-runtime"))
    implementation(project(":player-abstraction"))
    implementation(project(":common"))

    // HTTP
    implementation(libs.nicehttp)
    implementation(libs.newpipeextractor)
    implementation("com.squareup.okhttp3:okhttp:4.12.0")
    implementation("com.squareup.okhttp3:okhttp-dnsoverhttps:4.12.0")

    // JSON
    implementation("com.google.code.gson:gson:2.11.0") // Required for plugins using JsonParser.parseString (matches Android app)
    implementation("com.fasterxml.jackson.module:jackson-module-kotlin:2.18.3")
    implementation(kotlin("reflect")) // Required for Jackson to deserialize plugin Kotlin data classes
    implementation("org.json:json:20240303") // Required for plugins using org.json (natively included on Android)

    // Coroutines (swing provides Dispatchers.Main on desktop JVM)
    val coroutinesVersion = "1.10.2"
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:$coroutinesVersion")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-swing:$coroutinesVersion")

    // Desktop counterpart of Android's WebView system (now native CDP).
    // Android's built-in AES-GCM crypto is not available on desktop JVM.
    implementation("org.bouncycastle:bcprov-jdk18on:1.77")
    implementation("org.conscrypt:conscrypt-openjdk-uber:2.5.2")

    // JNA for MPV
    implementation("net.java.dev.jna:jna:5.14.0")
    implementation("net.java.dev.jna:jna-platform:5.14.0")

    // Compose Desktop UI
    implementation(compose.desktop.currentOs)
    implementation(compose.material3) // material3 already includes core icons
    implementation(compose.materialIconsExtended)
    implementation(compose.ui)
    implementation(compose.foundation)
    implementation("dev.chrisbanes.haze:haze:1.3.1")

    // Decompose Navigation
    implementation(libs.decompose)
    implementation(libs.decompose.extensions.compose)

    // Image loading
    implementation("io.coil-kt.coil3:coil-compose:3.0.0")
    implementation("io.coil-kt.coil3:coil-network-okhttp:3.0.0")
    implementation("io.coil-kt.coil3:coil-svg:3.0.0")

    // Logging
    implementation(libs.slf4j.api)
    implementation(libs.logback.classic)

    testImplementation(kotlin("test"))

    // SQLDelight
    implementation(libs.sqldelight.sqlite.driver)
    implementation(libs.sqldelight.coroutines.extensions)
}

// Compose Desktop application configuration
compose.desktop {
    application {
        mainClass = "com.lagradost.cloudstream3.desktop.MainKt"
        jvmArgs +=
            listOf(
                "-Xms256m",
                "-Xmx2048m",
                "-XX:+UseG1GC",
                "-XX:MaxGCPauseMillis=50",
                "-XX:CICompilerCount=2",
                "-Djava.security.manager=allow",
                "-Djava.net.preferIPv6Addresses=true",
                // NOTE: in this jpackage layout $APPDIR resolves to the app
                // module directory (lib/app), i.e. exactly where the resources
                // dir and the jars live. The Compose plugin's default
                // compose.application.resources.dir=$APPDIR/resources is
                // already correct; do NOT prepend lib/app here.
                "-Djava.library.path=\$APPDIR/resources/jni",
                "-Djna.library.path=\$APPDIR/resources/jni",
                // AppCDS: the packaging script trains app.jsa next to the jars;
                // -Xshare:auto falls back silently when the archive is missing
                // or the classpath changed, so this is always safe.
                "-XX:SharedArchiveFile=\$APPDIR/app.jsa",
                "-Xshare:auto",
                "-Dcloudstream.version=${project.findProperty("APP_VERSION")}",
                "-Dfile.encoding=UTF-8",
            )
        buildTypes.release.proguard {
            isEnabled.set(false)
        }

        nativeDistributions {
            // Linux packages (TAR/DEB/RPM/AppImage) are built by
            // desktop-app/packaging/build-linux-packages.sh — no native installer format needed here
            packageName = "CloudStream-Desktop"
            // jpackage STRICTLY requires version to be numeric (e.g. 0.1.5). Strip any -beta or -pre-alpha suffixes.
            packageVersion = project.findProperty("APP_VERSION")?.toString()?.substringBefore('-') ?: "0.0.0"
            description = "CloudStream Desktop Client"
            vendor = "CloudStream"
            includeAllModules = false
            modules(*requiredRuntimeModules.toTypedArray())
            appResourcesRootDir.set(project.layout.projectDirectory.dir("appResources"))

            windows {
                iconFile.set(project.file("src/main/resources/app_icon.ico"))
                menuGroup = "CloudStream Desktop"
                upgradeUuid = "d7e9b04f-723a-4467-84df-fcf470c1ae02"
                shortcut = true // Creates a Desktop shortcut during install
                perUserInstall = true // Installs per-user, avoids needing admin rights
            }

            linux {
                packageName = "CloudStream"
                // RGBA (rounded) icon for the launcher; app_icon.png is a
                // palette PNG without alpha and renders square in DE launchers.
                iconFile.set(project.file("src/main/resources/linux_icon.png"))
            }
        }
    }
}

tasks.matching { it.name == "run" }.configureEach {
    val runTask = this as JavaExec
    val nativeJniDir = if (isLinuxHost) "appResources/linux/jni" else "appResources/windows/jni"
    val nativeMpvDir = if (isLinuxHost) "appResources/linux/mpv" else "appResources/windows/mpv"
    runTask.jvmArgs(
        "-Xms256m",
        "-Xmx2048m",
        "-XX:+UseG1GC",
        "-XX:MaxGCPauseMillis=50",
        "-XX:CICompilerCount=2",
        "-Djna.library.path=${project.file(nativeMpvDir).absolutePath}",
        "-Djava.library.path=${project.file(nativeJniDir).absolutePath}",
        "-Dcloudstream.version=${project.findProperty("APP_VERSION")}",
    )
}

tasks.named("processResources") {
    if (isLinuxHost) dependsOn(linuxNativeBridge)
}

tasks.withType<Test> {
    useJUnitPlatform {
        // Exclude integration tests that require native binaries (e.g. libmpv-2.dll)
        // Run them manually with: ./gradlew :desktop-app:test -Dtags=native
        excludeTags("native")
    }
}

tasks.register<JavaExec>("runTestWebViewPlayer") {
    mainClass.set("com.lagradost.cloudstream3.desktop.test.TestWebViewPlayerKt")
    classpath = sourceSets["main"].runtimeClasspath
    jvmArgs("-Djava.library.path=appResources/windows/jni", "-Djna.library.path=appResources/windows/mpv")
}

tasks.register<JavaExec>("runTestMpvPlayer") {
    mainClass.set("com.lagradost.cloudstream3.desktop.test.TestMpvPlayerKt")
    classpath = sourceSets["main"].runtimeClasspath
    jvmArgs("-Djna.library.path=appResources/windows/mpv")
}
