package info.cemu.cemu.emulation

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import info.cemu.cemu.R
import info.cemu.cemu.common.ui.localization.tr

/**
 * Keeps the emulation process out of reach of the low memory killer while a game runs.
 *
 * Without it Android treats the process as cached as soon as the screen turns off or the device is
 * folded, which on the AYN Thor raises its oom_score_adj from 0 to 700. The emulation holds well
 * over a gigabyte, so it is the first process to be killed and the game is gone when the device is
 * opened again.
 */
class EmulationService : Service() {
    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val notificationManager = getSystemService(NotificationManager::class.java)
        if (notificationManager.getNotificationChannel(CHANNEL_ID) == null) {
            notificationManager.createNotificationChannel(
                NotificationChannel(
                    CHANNEL_ID,
                    tr("Running game"),
                    NotificationManager.IMPORTANCE_LOW,
                ).apply {
                    description = tr("Shown while a game is running")
                    setShowBadge(false)
                }
            )
        }

        val gameName = intent?.getStringExtra(EXTRA_GAME_NAME)
        val contentIntent = PendingIntent.getActivity(
            this,
            0,
            Intent(this, EmulationActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT or Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE,
        )
        val notification: Notification = NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.mipmap.ic_launcher)
            .setContentTitle(gameName ?: tr("Running game"))
            .setContentIntent(contentIntent)
            .setOngoing(true)
            .setShowWhen(false)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .build()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
        return START_NOT_STICKY
    }

    companion object {
        private const val CHANNEL_ID = "emulation"
        private const val NOTIFICATION_ID = 1
        private const val EXTRA_GAME_NAME = "gameName"

        fun start(context: Context, gameName: String?) {
            val intent = Intent(context, EmulationService::class.java)
                .putExtra(EXTRA_GAME_NAME, gameName)
            context.startForegroundService(intent)
        }

        fun stop(context: Context) {
            context.stopService(Intent(context, EmulationService::class.java))
        }
    }
}
