package com.example.duckusb

import android.app.Application
import android.os.Bundle
import de.wwb.android.wbosed.service.WbService
import de.wwb.android.wbosed.service.WbServiceHelper
import java.util.concurrent.CopyOnWriteArraySet

/**
 * Holds the framework service for the UI.
 *
 * Ported from DuckUSB (github.com/Bouteillepleine/DuckUSB); the only change is the package of
 * the service classes — Vector ships the same libxposed service library as
 * de.wwb.android.wbosed.service, so WbServiceHelper/WbService replace
 * XposedServiceHelper/XposedService one for one.
 *
 * The binder does not arrive synchronously — it is delivered to the .WbService provider inside
 * this process and handed on from a binder thread, which may be before or after the first
 * activity is created. WbServiceHelper keeps room for exactly ONE listener process wide, so the
 * Application registers itself and fans out to whoever is on screen.
 *
 * `service == null` is the honest answer to "is the module active": a module is loaded into its
 * own app process whether or not it is scoped, so a self-hook would prove nothing. What proves
 * the framework is live is that the daemon found the .WbService provider and pushed a binder.
 */
class DuckUsbApp : Application(), WbServiceHelper.OnServiceListener {

    companion object {
        /** Null until the framework binds, and again if it dies. */
        @Volatile
        var service: WbService? = null
            private set

        private val listeners = CopyOnWriteArraySet<(WbService?) -> Unit>()

        /** Registers [l] and calls it immediately with the current state. */
        fun addListener(l: (WbService?) -> Unit) {
            listeners.add(l)
            l(service)
        }

        fun removeListener(l: (WbService?) -> Unit) = listeners.remove(l)

        private fun dispatch(svc: WbService?) = listeners.forEach { it(svc) }
    }

    override fun onCreate() {
        super.onCreate()
        WbServiceHelper.registerListener(this)
    }

    override fun onServiceBind(service: WbService) {
        Companion.service = service
        dispatch(service)
    }

    override fun onServiceDied(service: WbService) {
        Companion.service = null
        dispatch(null)
    }
}
