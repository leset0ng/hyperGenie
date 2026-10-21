package ng.lst.hha

import android.util.Log
import io.github.libxposed.api.XposedModule
import io.github.libxposed.api.XposedModuleInterface.HotReloadedParam
import io.github.libxposed.api.XposedModuleInterface.HotReloadingParam
import io.github.libxposed.api.XposedModuleInterface.ModuleLoadedParam
import io.github.libxposed.api.XposedModuleInterface.PackageLoadedParam
import io.github.libxposed.api.XposedModuleInterface.SystemServerStartingParam

/**
 * Single module entry, referenced by META-INF/xposed/java_init.list.
 *
 * The framework instantiates this class reflectively, so keep the constructor empty
 * and avoid heavy work before [onModuleLoaded].
 */
class HookEntry : XposedModule() {

    private var centralBridge: CentralBridge? = null

    override fun onSystemServerStarting(param: SystemServerStartingParam) {
        runCatching {
            CentralBridge(this, param.classLoader).also { it.install() }
        }.onSuccess {
            centralBridge = it
        }.onFailure {
            log(Log.ERROR, TAG, "central content bridge unavailable; keeping stock animation", it)
        }
    }

    override fun onModuleLoaded(param: ModuleLoadedParam) {
        log(Log.INFO, TAG, "loaded in ${param.processName} (systemServer=${param.isSystemServer})")
    }

    override fun onPackageLoaded(param: PackageLoadedParam) {
        log(Log.DEBUG, TAG, "packageLoaded ${param.packageName} first=${param.isFirstPackage}")
    }

    /**
     * Runs in the *new* generation after a hot reload (API 102+).
     * The default implementation drops every hook installed by the old generation;
     * reinstall or replace them here instead if they should survive.
     */
    override fun onHotReloaded(param: HotReloadedParam) {
        super.onHotReloaded(param)
        log(Log.INFO, TAG, "hot reloaded, old hooks=${param.oldHookHandles.size}")
    }

    /** Retire worker requests and detach effects before replacing bridge hooks. */
    override fun onHotReloading(param: HotReloadingParam): Boolean {
        centralBridge?.close()
        centralBridge = null
        return true
    }

    private companion object {
        const val TAG = "HyperHomeAnimation"
    }
}
