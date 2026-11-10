package info.cemu.cemu

import android.app.Activity
import android.app.Application
import android.os.Bundle
import info.cemu.cemu.common.android.context.internalFolder
import info.cemu.cemu.common.settings.AppSettingsStore
import info.cemu.cemu.common.ui.localization.setLanguage
import info.cemu.cemu.common.ui.localization.setTranslations
import info.cemu.cemu.nativeinterface.NativeActiveSettings.initializeActiveSettings
import info.cemu.cemu.nativeinterface.NativeActiveSettings.setInternalDir
import info.cemu.cemu.nativeinterface.NativeActiveSettings.setNativeLibDir
import info.cemu.cemu.nativeinterface.NativeEmulation.initializeEmulation
import info.cemu.cemu.nativeinterface.NativeEmulation.setDPI
import info.cemu.cemu.nativeinterface.NativeFiles
import info.cemu.cemu.nativeinterface.NativeInput
import info.cemu.cemu.nativeinterface.NativeGraphicPacks.refreshGraphicPacks
import info.cemu.cemu.nativeinterface.NativeLogging.crashLog
import info.cemu.cemu.nativeinterface.NativeSwkbd.initializeSwkbd
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.runBlocking
import java.io.File
import java.io.IOException
import java.io.PrintWriter
import java.io.StringWriter
import java.util.regex.Pattern

class CemuApplication : Application() {
    override fun onCreate() {
        super.onCreate()

        configureExceptionHandler()

        AppSettingsStore.init(this)

        NativeFiles.initialize(contentResolver)

        initializeTranslations()

        initializeCemu()

        saveDataFiles()

        pauseInputWhileInBackground()
    }

    // Every process of the app polls the controllers. The release build runs the emulation in its own process, so
    // without this the process of the game list kept polling a thousand times per second while a game runs.
    private fun pauseInputWhileInBackground() {
        registerActivityLifecycleCallbacks(object : ActivityLifecycleCallbacks {
            private var startedActivities = 0

            override fun onActivityStarted(activity: Activity) {
                if (startedActivities++ == 0) {
                    NativeInput.setUpdatesPaused(false)
                }
            }

            override fun onActivityStopped(activity: Activity) {
                if (--startedActivities == 0) {
                    NativeInput.setUpdatesPaused(true)
                }
            }

            override fun onActivityCreated(activity: Activity, savedInstanceState: Bundle?) {}
            override fun onActivityResumed(activity: Activity) {}
            override fun onActivityPaused(activity: Activity) {}
            override fun onActivitySaveInstanceState(activity: Activity, outState: Bundle) {}
            override fun onActivityDestroyed(activity: Activity) {}
        })
    }

    private fun initializeTranslations() {
        setTranslations(this)

        val language = runBlocking {
            AppSettingsStore.dataStore.data.map { it.guiSettings.language }.first()
        }

        setLanguage(language, this)
    }

    private fun saveDataFiles() {
        val dataFolder = File(internalCemuDataFolder)

        if (!dataFolder.exists() && !dataFolder.mkdirs()) {
            return
        }

        val hashFileName = "hash.txt"
        val hashFile = dataFolder.resolve(hashFileName)
        val oldHash = if (hashFile.isFile) hashFile.readText() else "invalid"

        val newHash = try {
            assets.open(hashFileName).use { it.reader().readText() }
        } catch (_: IOException) {
            return
        }

        if (oldHash == newHash) {
            return
        }

        dataFolder.deleteRecursively()
        dataFolder.mkdirs()
        dataFolder.resolve(hashFileName).writeText(newHash)

        fun traverseAssets(path: String = ""): Iterator<String> = iterator {
            val assetFiles = assets.list(path) ?: return@iterator

            if (assetFiles.isEmpty()) {
                yield(path)
            }

            for (assetFile in assetFiles) {
                val assetPath = path + (if (path == "") "" else "/") + assetFile
                for (file in traverseAssets(assetPath)) {
                    yield(file)
                }
            }
        }

        val filePatterns = arrayOf(
            Pattern.compile("gameProfiles/.*"),
            Pattern.compile("resources/.*"),
        )

        fun isFileValid(file: String): Boolean {
            return filePatterns.any { pattern -> pattern.matcher(file).matches() }
        }

        for (assetFile in traverseAssets()) {
            if (!isFileValid(assetFile)) {
                continue
            }

            val outFile = dataFolder.resolve(assetFile)
            outFile.parentFile?.mkdirs()
            assets.open(assetFile)
                .use { asset -> outFile.outputStream().use { out -> asset.copyTo(out) } }
        }
    }

    private fun configureExceptionHandler() {
        if (DefaultUncaughtExceptionHandler == null) {
            DefaultUncaughtExceptionHandler = Thread.getDefaultUncaughtExceptionHandler()
        }
        Thread.setDefaultUncaughtExceptionHandler { thread: Thread, exception: Throwable ->
            val stringWriter = StringWriter()
            val printWriter = PrintWriter(stringWriter)
            exception.printStackTrace(printWriter)
            val stacktrace = stringWriter.toString()
            crashLog(stacktrace)
            DefaultUncaughtExceptionHandler!!.uncaughtException(
                thread,
                exception,
            )
        }
    }

    private fun initializeCemu() {
        val displayMetrics = resources.displayMetrics
        setDPI(displayMetrics.density)
        initializeActiveSettings(
            userDataPath = internalCemuUserFolder,
            configPath = internalCemuUserFolder,
            dataPath = internalCemuDataFolder,
            cachePath = internalCemuUserFolder,
        )
        setNativeLibDir(applicationInfo.nativeLibraryDir)
        setInternalDir(dataDir.absolutePath)
        initializeEmulation()
        initializeSwkbd()
        refreshGraphicPacks()
    }

    private val internalCemuDataFolder: String
        get() = internalFolder().resolve("data").toString()

    private val internalCemuUserFolder: String
        get() = internalFolder().toString()

    companion object {
        init {
            System.loadLibrary("CemuAndroid")
        }

        private var DefaultUncaughtExceptionHandler: Thread.UncaughtExceptionHandler? = null
    }
}
