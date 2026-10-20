package ng.lst.hha

import android.os.Handler
import android.os.Looper
import android.util.Log
import io.github.libxposed.api.XposedInterface
import io.github.libxposed.api.XposedInterface.HookHandle
import io.github.libxposed.api.XposedModule
import io.github.libxposed.api.XposedModuleInterface.HotReloadedParam
import io.github.libxposed.api.XposedModuleInterface.HotReloadingParam
import io.github.libxposed.api.XposedModuleInterface.ModuleLoadedParam
import io.github.libxposed.api.XposedModuleInterface.PackageLoadedParam
import io.github.libxposed.api.XposedModuleInterface.PackageReadyParam

/**
 * Single module entry, referenced by META-INF/xposed/java_init.list.
 *
 * Lifecycle: onModuleLoaded -> onPackageLoaded -> onPackageReady.
 * The framework instantiates this class reflectively, so keep the constructor empty
 * and avoid heavy work before [onModuleLoaded].
 */
class HookEntry : XposedModule() {

    private var installed = false
    private var settingsProcess = false
    private var settingsHooks: SettingsHooks? = null

    override fun onModuleLoaded(param: ModuleLoadedParam) {
        settingsProcess = param.processName == SETTINGS_PACKAGE
        log(Log.INFO, TAG, "loaded in ${param.processName} (systemServer=${param.isSystemServer})")
    }

    override fun onPackageLoaded(param: PackageLoadedParam) {
        log(Log.DEBUG, TAG, "packageLoaded ${param.packageName} first=${param.isFirstPackage}")
    }

    override fun onPackageReady(param: PackageReadyParam) {
        if (param.packageName == SETTINGS_PACKAGE && settingsProcess) {
            installSettingsHooks()
            return
        }
        // A process may load several packages; only patch the launcher once.
        if (installed) return
        if (param.packageName != TARGET_PACKAGE) return
        installed = true

        val loader = param.classLoader
        log(Log.INFO, TAG, "packageReady ${param.packageName}, installing hooks")

        // Android 17 / HyperOS 4 ships the launcher without any dex, so this class
        // never loads there and the real work happens in libhha_native.so
        // (see META-INF/xposed/native_init.list). These Java hooks only apply to
        // older HyperOS builds that still ship a dex-based launcher.

        // TODO: the class/method names below are placeholders. Verify them against the
        // target build (obfuscation changes between HyperOS releases) before hooking.
        runCatching {
            val clazz = Class.forName("com.miui.home.launcher.Launcher", false, loader)
            val method = clazz.getDeclaredMethod("onResume")
            val handle: HookHandle = hook(method)
                .setPriority(XposedInterface.PRIORITY_DEFAULT)
                .setExceptionMode(XposedInterface.ExceptionMode.PROTECTIVE)
                .setId("launcher-onResume")
                .intercept { chain ->
                    log(Log.DEBUG, TAG, "before onResume")
                    val result = chain.proceed()
                    log(Log.DEBUG, TAG, "after onResume")
                    result
                }
            log(Log.INFO, TAG, "hooked onResume -> ${handle.id}")
        }.onFailure {
            log(Log.ERROR, TAG, "hook failed: ${it.message}", it)
        }
    }

    private fun installSettingsHooks() {
        if (settingsHooks != null) return
        runCatching {
            SettingsHooks(this).also { it.install() }
        }.onSuccess {
            settingsHooks = it
        }.onFailure {
            log(Log.ERROR, TAG, "Settings content hooks unavailable: ${it.message}", it)
        }
    }

    /**
     * Runs in the *new* generation after a hot reload (API 102+).
     * The default implementation drops every hook installed by the old generation;
     * reinstall or replace them here instead if they should survive.
     */
    override fun onHotReloaded(param: HotReloadedParam) {
        installed = false
        settingsHooks = null
        settingsProcess = param.processName == SETTINGS_PACKAGE
        super.onHotReloaded(param)
        if (settingsProcess) {
            Handler(Looper.getMainLooper()).post { installSettingsHooks() }
        }
        log(Log.INFO, TAG, "hot reloaded, old hooks=${param.oldHookHandles.size}")
    }

    /** Runs in the *old* generation. Release VRI state before replacing hooks. */
    override fun onHotReloading(param: HotReloadingParam): Boolean {
        settingsHooks?.close()
        settingsHooks = null
        return true
    }

    private companion object {
        const val TAG = "HyperHomeAnimation"
        const val TARGET_PACKAGE = "com.miui.home"
        const val SETTINGS_PACKAGE = "com.android.settings"
    }
}
