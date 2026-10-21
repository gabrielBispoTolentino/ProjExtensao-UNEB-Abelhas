// Serviço em primeiro plano do Android: mantém o app rodando quando ele sai
// da tela (conexões Bluetooth com as placas, despejo do SD, envio ao
// Supabase) e mostra o andamento numa notificação fixa. Sem ele, o Android
// congela ou fecha o processo pouco depois de o app sair da tela.
//
// O C++ (androidbackground.cpp) chama update() a cada mudança de estado: a
// primeira chamada sobe o serviço, as outras só trocam o texto. Tocar na
// notificação abre o app; o botão "Encerrar" fecha o app de vez (ver quitApp()).

package com.abelhaspetterson.sensormonitor;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.graphics.drawable.Icon;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Process;
import android.util.Log;

public class MonitorService extends Service
{
    private static final String TAG = "BeeDuino";
    private static final String CHANNEL_ID = "monitor";
    private static final int NOTIFICATION_ID = 1;
    private static final int NOTIFICATION_PERMISSION_REQUEST = 4201;
    private static final String ACTION_QUIT = "com.abelhaspetterson.sensormonitor.QUIT";
    private static final int QUIT_GRACE_MS = 1500;
    private static final int EXIT_DELAY_MS = 300;

    private static String sText = "";
    private static boolean sForeground = false;   // startForeground() já deu certo

    // Registrado pelo C++: fecha o app (QCoreApplication::quit).
    private static native void nativeQuitRequested();

    // ── Chamado pelo C++ ────────────────────────────────────────────────────
    public static void update(Context context, String text)
    {
        sText = text;
        if (sForeground) {
            NotificationManager manager = context.getSystemService(NotificationManager.class);
            manager.notify(NOTIFICATION_ID, buildNotification(context));
            return;
        }
        try {
            context.startForegroundService(new Intent(context, MonitorService.class));
        } catch (RuntimeException e) {
            // Ex.: o app já está em segundo plano (Android 12+ não deixa subir
            // o serviço daí); tenta de novo na próxima mudança de estado.
            Log.w(TAG, "Could not start the background service: " + e);
        }
    }

    public static void stop(Context context)
    {
        context.stopService(new Intent(context, MonitorService.class));
    }

    // O app terminou (main() do Qt vai retornar): fecha a tarefa e termina o
    // processo logo depois. Deixado sozinho, o processo morre ~1 s depois com
    // SIGABRT (threads de desenho do Android usando um mutex já destruído na
    // saída), e reabrir num processo velho do Qt não funciona.
    public static void finishAndExit(final Context context)
    {
        stop(context);
        final Context app = context.getApplicationContext();
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            for (ActivityManager.AppTask task : app.getSystemService(ActivityManager.class).getAppTasks()) {
                task.finishAndRemoveTask();
            }
            Process.killProcess(Process.myPid());
        }, EXIT_DELAY_MS);
    }

    // Android 13+: sem esta permissão o serviço roda, mas a notificação não
    // aparece. Pedida depois da do Bluetooth (dois pedidos ao mesmo tempo, o
    // Android descarta um).
    public static void requestNotificationPermission(Context context)
    {
        if (Build.VERSION.SDK_INT < 33 || !(context instanceof Activity)) {
            return;
        }
        final String permission = "android.permission.POST_NOTIFICATIONS";
        if (context.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED) {
            return;
        }
        ((Activity) context).requestPermissions(new String[] { permission },
                                                NOTIFICATION_PERMISSION_REQUEST);
    }

    // ── Serviço ─────────────────────────────────────────────────────────────
    @Override
    public int onStartCommand(Intent intent, int flags, int startId)
    {
        if (intent != null && ACTION_QUIT.equals(intent.getAction())) {
            quitApp();
            return START_NOT_STICKY;
        }

        NotificationChannel channel = new NotificationChannel(
            CHANNEL_ID, "Monitoramento das colmeias", NotificationManager.IMPORTANCE_LOW);
        channel.setDescription("Andamento do Bluetooth e do envio dos registros à nuvem");
        channel.setShowBadge(false);
        getSystemService(NotificationManager.class).createNotificationChannel(channel);

        try {
            if (Build.VERSION.SDK_INT >= 29) {
                startForeground(NOTIFICATION_ID, buildNotification(this),
                                ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE);
            } else {
                startForeground(NOTIFICATION_ID, buildNotification(this));
            }
            sForeground = true;
        } catch (RuntimeException e) {
            Log.w(TAG, "startForeground failed: " + e);
            stopSelf();
        }
        return START_NOT_STICKY;
    }

    // "Encerrar": o Qt fecha o app (e o banco local); ao terminar, o C++ chama
    // finishAndExit(). O temporizador daqui é a garantia se o Qt não terminar.
    private void quitApp()
    {
        nativeQuitRequested();
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            for (ActivityManager.AppTask task : getSystemService(ActivityManager.class).getAppTasks()) {
                task.finishAndRemoveTask();
            }
            stopSelf();
            Process.killProcess(Process.myPid());
        }, QUIT_GRACE_MS);
    }

    @Override
    public void onDestroy()
    {
        sForeground = false;
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent)
    {
        return null;
    }

    // ── Notificação ─────────────────────────────────────────────────────────
    private static Notification buildNotification(Context context)
    {
        final String packageName = context.getPackageName();
        final int flags = PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT;

        Intent open = context.getPackageManager().getLaunchIntentForPackage(packageName);
        PendingIntent openApp = PendingIntent.getActivity(context, 0, open, flags);
        Intent quit = new Intent(context, MonitorService.class).setAction(ACTION_QUIT);
        PendingIntent quitApp = PendingIntent.getService(context, 1, quit, flags);

        // Pelo nome: a classe R gerada fica em outro pacote conforme o build.
        int icon = context.getResources().getIdentifier("ic_notification", "drawable", packageName);

        return new Notification.Builder(context, CHANNEL_ID)
            .setSmallIcon(icon)
            .setContentTitle("Monitorando as colmeias")
            .setContentText(sText)
            .setStyle(new Notification.BigTextStyle().bigText(sText))
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setShowWhen(false)
            .setCategory(Notification.CATEGORY_SERVICE)
            .setContentIntent(openApp)
            .addAction(new Notification.Action.Builder((Icon) null, "Encerrar", quitApp).build())
            .build();
    }
}
