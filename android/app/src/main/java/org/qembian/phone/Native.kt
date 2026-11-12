package org.qembian.phone

import android.view.Surface

/** libqemu-syborg.so (android/native/syborg_android.c): QEMU with our board, running in this process. */
object Native {
    init {
        System.loadLibrary("qemu-syborg")
    }

    /** Starts QEMU on its own thread with these arguments (argv[1..]); once per process. */
    @JvmStatic external fun start(args: Array<String>)

    /** A touch on the picture: x, y in 0..0x7fff of the phone's screen. */
    @JvmStatic external fun touch(id: Int, x: Int, y: Int, down: Boolean)

    /** A phone key by its QEMU key code name (the bundle's key list: "f8", "f2", ...). */
    @JvmStatic external fun key(qcode: String, down: Boolean)

    /** The window the phone's screen is shown in (null: it went away). */
    @JvmStatic external fun setWindow(surface: Surface?)

    /** Switches the phone off: QEMU shuts down and the process ends. */
    @JvmStatic external fun quit()
}
