package org.matrix.vector.impl.hookers

import de.wwb.android.wbosed.api.WbInterface
import org.matrix.vector.util.Utils

/**
 * Intercepts uncaught exceptions in the framework to provide diagnostic logging before the process
 * completely terminates.
 */
object CrashDumpHooker : WbInterface.Hooker {
    override fun intercept(chain: WbInterface.Chain): Any? {
        try {
            val throwable = chain.args.firstOrNull() as? Throwable
            if (throwable != null) {
                Utils.logE("Crash unexpectedly", throwable)
            }
        } catch (ignored: Throwable) {}
        return chain.proceed()
    }
}
