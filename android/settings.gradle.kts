// qembian phone for Android (android/README.md): one app, one Activity, QEMU in libqemu-syborg.so
pluginManagement {
    repositories { google(); mavenCentral(); gradlePluginPortal() }
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories { google(); mavenCentral() }
}
rootProject.name = "qembian-phone"
include(":app")
