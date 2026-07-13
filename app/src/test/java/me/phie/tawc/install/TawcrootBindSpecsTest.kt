package me.phie.tawc.install

import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * RO/RW split of [TawcrootMethod.bindSpecs]: the system-partition
 * (libhybris dlopen) binds emit tawcroot's 3-field `:ro` form; every
 * other built-in and the external binds keep the 2-field RW form.
 * Exact-list assertions also pin bind order (built-ins before
 * external so user binds can't shadow the system/share set).
 */
class TawcrootBindSpecsTest {
    private val share = "/data/data/me.phie.tawc/files/share"
    private val hybrisDirs =
        listOf("/apex", "/vendor", "/system", "/system_ext", "/linkerconfig")

    @Test
    fun systemBindsRoOthersRw() {
        val args = TawcrootMethod.bindSpecs(
            tawcShare = share,
            libhybrisDirs = hybrisDirs,
            externalBinds = listOf(ExternalBind("/storage/emulated/0", "/home/android")),
            andoHostDir = "/data/data/me.phie.tawc/files/ando/arch",
        ).map { it.arg() }
        assertEquals(
            listOf(
                "/dev:/dev",
                "/proc:/proc",
                "/sys:/sys",
                "/apex:/apex:ro",
                "/vendor:/vendor:ro",
                "/system:/system:ro",
                "/system_ext:/system_ext:ro",
                "/linkerconfig:/linkerconfig:ro",
                "$share:/usr/share/tawc",
                "/data/data/me.phie.tawc/files/ando/arch:/run/tawc-ando",
                "$share/xtmp/.X11-unix:/tmp/.X11-unix",
                "/storage/emulated/0:/home/android",
            ),
            args,
        )
    }

    @Test
    fun andoDisabledOmitsItsBind() {
        val args = TawcrootMethod.bindSpecs(share, hybrisDirs, emptyList(), null).map { it.arg() }
        assertEquals(
            listOf(
                "/dev:/dev",
                "/proc:/proc",
                "/sys:/sys",
                "/apex:/apex:ro",
                "/vendor:/vendor:ro",
                "/system:/system:ro",
                "/system_ext:/system_ext:ro",
                "/linkerconfig:/linkerconfig:ro",
                "$share:/usr/share/tawc",
                "$share/xtmp/.X11-unix:/tmp/.X11-unix",
            ),
            args,
        )
    }
}
