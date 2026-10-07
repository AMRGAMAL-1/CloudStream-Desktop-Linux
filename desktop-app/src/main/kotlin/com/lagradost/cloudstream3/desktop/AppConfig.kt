package com.lagradost.cloudstream3.desktop

object AppConfig {
    val APP_VERSION = System.getProperty("cloudstream.version", "0.0.0")

    /** The upstream Windows release channel. Linux never reads this channel. */
    const val WINDOWS_UPDATE_REPO = "errorcode26/CS3-desktop-client-unofficial"

    /**
     * The Linux fork's release repository.
     *
     * Linux updates are published only in this fork, with tags such as
     * `linux-v0.1.10`, so the Linux client never offers a Windows release.
     * The value can be overridden at runtime with
     * -Dcloudstream.linux.update.repo=owner/repository (used by dev builds).
     */
    val LINUX_UPDATE_REPO = System.getProperty(
        "cloudstream.linux.update.repo",
        "AMRGAMAL-1/CloudStream-Desktop-Linux",
    )
}
