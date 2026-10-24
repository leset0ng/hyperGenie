// SPDX-License-Identifier: Apache-2.0
import android.content.ComponentName;
import android.os.Bundle;
import android.os.IBinder;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.KeyEvent;
import android.view.MotionEvent;
import java.io.BufferedReader;
import java.io.FileReader;
import java.io.InputStreamReader;
import java.lang.reflect.Method;
import java.util.ArrayDeque;
import java.util.Iterator;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import org.json.JSONArray;
import org.json.JSONObject;

/** Persistent, display-scoped touch injection for the explicitly allowed emulator. */
public final class AvdGestureInput {
    private final Object inputManager;
    private final Method injectInput;
    private final Method setDisplayId;
    private final Object taskManager;
    private final Method focusedTask;
    private final Object windowManager;
    private final Method focusedWindow;
    private final int displayId;
    private final String launcherPackage;
    private final int focusTimeoutMs;
    private final int sampleMs;
    private final String focusMode;
    private long timelineShift;
    private long lastTouchDownUptime;
    private final ArrayDeque<JSONObject> nativeCloses = new ArrayDeque<>();
    private Process nativeLogcat;
    private String nativeReaderFailure;
    private static final Pattern NATIVE_CLOSE = Pattern.compile(
            "^\\s*(\\d+\\.\\d+)\\s+\\d+\\s+\\d+\\s+I\\s+Genie\\s*:\\s*"
            + "start layer=(\\d+) direction=close rect=.*?->\\("
            + "(-?\\d+(?:\\.\\d+)?),(-?\\d+(?:\\.\\d+)?) "
            + "(\\d+(?:\\.\\d+)?)x(\\d+(?:\\.\\d+)?)\\)(?:\\s|$)");

    private AvdGestureInput(JSONObject plan) throws Exception {
        displayId = plan.getInt("display");
        if (displayId != 0 || !"emulator-5580".equals(plan.getString("serial"))) {
            throw new IllegalArgumentException("Only emulator-5580 display 0 is permitted");
        }
        Class<?> properties = Class.forName("android.os.SystemProperties");
        String qemu = (String) properties.getMethod("get", String.class).invoke(null, "ro.kernel.qemu");
        if (!"1".equals(qemu)) throw new IllegalStateException("Refusing a non-emulator target");
        launcherPackage = plan.getString("launcher_package");
        focusTimeoutMs = plan.getInt("focus_timeout_ms");
        sampleMs = plan.getInt("sample_ms");
        focusMode = plan.optString("focus_mode", "window");
        if (!"window".equals(focusMode) && !"scheduled".equals(focusMode)) {
            throw new IllegalArgumentException("Unknown focus mode: " + focusMode);
        }
        Class<?> managerClass;
        try {
            managerClass = Class.forName("android.hardware.input.InputManagerGlobal");
        } catch (ClassNotFoundException olderAndroid) {
            managerClass = Class.forName("android.hardware.input.InputManager");
        }
        inputManager = managerClass.getMethod("getInstance").invoke(null);
        injectInput = managerClass.getMethod("injectInputEvent", InputEvent.class, int.class);
        setDisplayId = InputEvent.class.getMethod("setDisplayId", int.class);
        Class<?> taskManagerClass = Class.forName("android.app.ActivityTaskManager");
        taskManager = taskManagerClass.getMethod("getService").invoke(null);
        focusedTask = Class.forName("android.app.IActivityTaskManager").getMethod("getFocusedRootTaskInfo");
        Object windowBinder = Class.forName("android.os.ServiceManager").getMethod("getService", String.class).invoke(null, "window");
        windowManager = Class.forName("android.view.IWindowManager$Stub").getMethod("asInterface", IBinder.class).invoke(null, windowBinder);
        focusedWindow = Class.forName("android.view.IWindowManager").getMethod("getFocusWindowInfo");
    }

    private static void emit(JSONObject value) {
        System.out.println(value.toString());
        System.out.flush();
    }

    private static void waitUntil(long deadline) {
        long remaining;
        while ((remaining = deadline - SystemClock.uptimeMillis()) > 0) SystemClock.sleep(remaining);
    }

    private void startNativeCloseReader() throws Exception {
        nativeLogcat = new ProcessBuilder("logcat", "-v", "monotonic", "-T", "1", "Genie:I", "*:S")
                .redirectErrorStream(true).start();
        Thread nativeCloseReader = new Thread(() -> {
            try (BufferedReader reader = new BufferedReader(new InputStreamReader(nativeLogcat.getInputStream()))) {
                String line;
                while ((line = reader.readLine()) != null) {
                    long observed = SystemClock.uptimeMillis();
                    Matcher match = NATIVE_CLOSE.matcher(line);
                    if (!match.find()) continue;
                    double width = Double.parseDouble(match.group(5));
                    double height = Double.parseDouble(match.group(6));
                    if (width <= 0 || height <= 0) continue;
                    JSONObject close = new JSONObject()
                            .put("native_close_uptime_ms", Math.round(Double.parseDouble(match.group(1)) * 1000))
                            .put("native_close_layer", Long.parseLong(match.group(2)))
                            .put("observed_uptime_ms", observed)
                            .put("center_x", Double.parseDouble(match.group(3)) + width / 2)
                            .put("center_y", Double.parseDouble(match.group(4)) + height / 2);
                    synchronized (nativeCloses) {
                        if (nativeCloses.size() == 64) nativeCloses.removeFirst();
                        nativeCloses.addLast(close);
                        nativeCloses.notifyAll();
                    }
                }
                synchronized (nativeCloses) {
                    nativeReaderFailure = "Native close logcat reader reached EOF";
                }
            } catch (Exception error) {
                synchronized (nativeCloses) {
                    nativeReaderFailure = error.toString();
                }
            } finally {
                synchronized (nativeCloses) {
                    nativeCloses.notifyAll();
                }
            }
        }, "NativeCloseLogcat");
        nativeCloseReader.setDaemon(true);
        nativeCloseReader.start();
    }

    private JSONObject awaitNativeClose(JSONObject action, long swipeDown) throws Exception {
        JSONObject gate = action.getJSONObject("native_close_gate");
        long begin = SystemClock.uptimeMillis();
        long timeout = begin + gate.getLong("timeout_ms");
        String failure;
        synchronized (nativeCloses) {
            while (true) {
                Iterator<JSONObject> records = nativeCloses.iterator();
                while (records.hasNext()) {
                    JSONObject close = records.next();
                    if (close.getLong("native_close_uptime_ms") < swipeDown) {
                        records.remove();
                        continue;
                    }
                    if (Math.hypot(close.getDouble("center_x") - gate.getDouble("center_x"),
                                   close.getDouble("center_y") - gate.getDouble("center_y"))
                            <= gate.getDouble("tolerance_px")) {
                        records.remove();
                        return close.put("type", "native_close_gate").put("label", action.getString("label"))
                                .put("dependency", gate.getString("dependency")).put("swipe_down_uptime_ms", swipeDown)
                                .put("gate_begin_uptime_ms", begin).put("match_wait_ms", SystemClock.uptimeMillis() - begin)
                                .put("requested_delay_ms", gate.getLong("delay_ms"))
                                .put("requested_uptime_ms", close.getLong("native_close_uptime_ms") + gate.getLong("delay_ms"))
                                .put("reader_delay_ms", close.getLong("observed_uptime_ms") - close.getLong("native_close_uptime_ms"));
                    }
                }
                if (nativeReaderFailure != null) {
                    failure = nativeReaderFailure;
                    break;
                }
                long remaining = timeout - SystemClock.uptimeMillis();
                if (remaining <= 0) {
                    failure = "Timed out waiting for this Phone native close";
                    break;
                }
                nativeCloses.wait(remaining);
            }
        }
        emit(new JSONObject().put("type", "native_close_gate_error").put("label", action.getString("label"))
             .put("dependency", gate.getString("dependency")).put("swipe_down_uptime_ms", swipeDown)
             .put("gate_begin_uptime_ms", begin).put("wait_ms", SystemClock.uptimeMillis() - begin)
             .put("message", failure));
        throw new IllegalStateException(failure + " before " + action.getString("label"));
    }

    private JSONObject readFocus() throws Exception {
        String component = "none";
        int focusedDisplay = -1;
        Object task = focusedTask.invoke(taskManager);
        if (task != null) {
            ComponentName top = (ComponentName) task.getClass().getField("topActivity").get(task);
            focusedDisplay = task.getClass().getField("displayId").getInt(task);
            component = top == null ? "none" : top.flattenToShortString();
        }
        Bundle window = (Bundle) focusedWindow.invoke(windowManager);
        return new JSONObject().put("task_component", component).put("display", focusedDisplay)
                .put("window_package", window == null ? "none" : window.getString("package_name", "none"))
                .put("window_visibility", window == null ? -1 : window.getInt("visibility", -1))
                .put("bundle_keys", window == null ? new JSONArray() : new JSONArray(window.keySet()));
    }

    private void requireLauncher(JSONObject action) throws Exception {
        long begin = SystemClock.uptimeMillis();
        JSONObject focus;
        do {
            focus = readFocus();
            // HyperOS can keep the closing app as the focused root task after its
            // window has yielded input to Home. Use WMS for the owning package;
            // the task snapshot only constrains the focused display.
            if (launcherPackage.equals(focus.getString("window_package"))
                    && focus.getInt("window_visibility") == 0 && focus.getInt("display") == displayId) {
                emit(focus.put("type", "focus").put("source", "oem-focused-window")
                     .put("focus_mode", focusMode).put("guarded", true).put("focus_sampled", true)
                     .put("label", action.getString("label")).put("uptime_ms", SystemClock.uptimeMillis())
                     .put("wait_ms", SystemClock.uptimeMillis() - begin));
                return;
            }
            SystemClock.sleep(8);
        } while (SystemClock.uptimeMillis() - begin < focusTimeoutMs);
        throw new IllegalStateException("Launcher focus timeout before " + action.getString("label")
                                        + "; focus=" + focus);
    }

    private long inject(InputEvent event, JSONObject record) throws Exception {
        setDisplayId.invoke(event, displayId);
        long begin = SystemClock.uptimeMillis();
        // ASYNC avoids waiting for the application to finish processing each MOVE.
        boolean accepted = (Boolean) injectInput.invoke(inputManager, event, 0);
        long end = SystemClock.uptimeMillis();
        emit(record.put("type", "input").put("display", displayId).put("event_uptime_ms", begin)
             .put("done_uptime_ms", end).put("injection_duration_ms", end - begin).put("accepted", accepted));
        if (!accepted) throw new IllegalStateException("Input event was rejected");
        return begin;
    }

    private void touch(JSONObject action, int motionAction, float x, float y, long downTime, long deadline) throws Exception {
        waitUntil(deadline);
        long now = SystemClock.uptimeMillis();
        MotionEvent.PointerProperties properties = new MotionEvent.PointerProperties();
        properties.id = 0;
        properties.toolType = MotionEvent.TOOL_TYPE_FINGER;
        MotionEvent.PointerCoords point = new MotionEvent.PointerCoords();
        point.x = x;
        point.y = y;
        point.pressure = motionAction == MotionEvent.ACTION_UP || motionAction == MotionEvent.ACTION_CANCEL ? 0 : 1;
        point.size = 1;
        // Keep the requested trajectory's monotonic timestamps when injection
        // is late, so queued MOVE/UP events retain their intended fling velocity.
        MotionEvent event = MotionEvent.obtain(downTime, deadline, motionAction, 1,
                new MotionEvent.PointerProperties[]{properties}, new MotionEvent.PointerCoords[]{point},
                0, 0, 1, 1, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0);
        try {
            long injected = inject(event, new JSONObject().put("label", action.getString("label")).put("action", MotionEvent.actionToString(motionAction))
                   .put("x", x).put("y", y).put("down_time_ms", downTime).put("scheduled_uptime_ms", deadline)
                   .put("motion_event_time_ms", deadline).put("lateness_ms", now - deadline));
            if (motionAction == MotionEvent.ACTION_DOWN) lastTouchDownUptime = injected;
        } finally {
            event.recycle();
        }
    }

    private void gesture(JSONObject action, long deadline) throws Exception {
        float x = (float) action.getDouble("x"), y = (float) action.getDouble("y");
        int duration = action.getInt("duration_ms");
        boolean swipe = "swipe".equals(action.getString("action"));
        float endX = swipe ? (float) action.getDouble("end_x") : x;
        float endY = swipe ? (float) action.getDouble("end_y") : y;
        long downTime = deadline;
        boolean finished = false;
        try {
            touch(action, MotionEvent.ACTION_DOWN, x, y, downTime, deadline);
            if (swipe) {
                for (int elapsed = sampleMs; elapsed < duration; elapsed += sampleMs) {
                    float progress = elapsed / (float) duration;
                    touch(action, MotionEvent.ACTION_MOVE, x + (endX - x) * progress,
                          y + (endY - y) * progress, downTime, deadline + elapsed);
                }
            }
            touch(action, MotionEvent.ACTION_UP, endX, endY, downTime, deadline + duration);
            finished = true;
        } finally {
            if (!finished) touch(action, MotionEvent.ACTION_CANCEL, endX, endY, downTime, SystemClock.uptimeMillis());
        }
    }

    private void homeKey(JSONObject action, long deadline) throws Exception {
        long downTime = SystemClock.uptimeMillis();
        int duration = action.getInt("duration_ms");
        for (int state : new int[]{KeyEvent.ACTION_DOWN, KeyEvent.ACTION_UP}) {
            long scheduled = deadline + (state == KeyEvent.ACTION_UP ? duration : 0);
            waitUntil(scheduled);
            KeyEvent event = new KeyEvent(downTime, SystemClock.uptimeMillis(), state, KeyEvent.KEYCODE_HOME,
                    0, 0, -1, 0, 0, InputDevice.SOURCE_KEYBOARD);
            inject(event, new JSONObject().put("label", action.getString("label"))
                   .put("action", state == KeyEvent.ACTION_DOWN ? "HOME_DOWN" : "HOME_UP")
                   .put("scheduled_uptime_ms", scheduled).put("down_time_ms", downTime));
        }
    }

    private void run(JSONObject plan) throws Exception {
        JSONArray actions = plan.getJSONArray("events");
        boolean gated = false;
        for (int index = 0; index < actions.length(); index++) {
            JSONObject action = actions.getJSONObject(index);
            if (!action.has("native_close_gate")) continue;
            JSONObject gate = action.getJSONObject("native_close_gate");
            JSONObject previous = index == 0 ? new JSONObject() : actions.getJSONObject(index - 1);
            if (!"scheduled".equals(focusMode) || !"tap".equals(action.optString("action"))
                    || !("phone".equals(action.optString("target")) || "settings".equals(action.optString("target")))
                    || !"swipe".equals(previous.optString("action")) || !"phone".equals(previous.optString("target"))
                    || !gate.getString("dependency").equals(previous.optString("label"))
                    || gate.getLong("delay_ms") < 0 || gate.getLong("timeout_ms") <= 0
                    || gate.getDouble("tolerance_px") <= 0) {
                throw new IllegalArgumentException("Native close gate requires the preceding Phone home swipe in scheduled mode");
            }
            gated = true;
        }
        try {
            if (gated) startNativeCloseReader();
            runActions(plan, actions);
        } finally {
            if (nativeLogcat != null) nativeLogcat.destroy();
        }
    }

    private void runActions(JSONObject plan, JSONArray actions) throws Exception {
        long begin = SystemClock.uptimeMillis();
        long base = begin + plan.getInt("start_delay_ms");
        emit(new JSONObject().put("type", "ready").put("uptime_ms", begin)
             .put("elapsed_realtime_ns", SystemClock.elapsedRealtimeNanos()).put("wall_time_ms", System.currentTimeMillis())
             .put("base_uptime_ms", base).put("pid", android.os.Process.myPid()).put("plan", plan));
        boolean firstTap = true;
        long lastPhoneSwipeDown = -1;
        String lastPhoneSwipeLabel = "";
        for (int index = 0; index < actions.length(); index++) {
            JSONObject action = actions.getJSONObject(index);
            long original = base + action.getLong("offset_ms");
            long scheduled = original + timelineShift;
            JSONObject gateMatch = null;
            if (action.has("native_close_gate")) {
                if (lastPhoneSwipeDown < 0 || !lastPhoneSwipeLabel.equals(
                        action.getJSONObject("native_close_gate").getString("dependency"))) {
                    throw new IllegalStateException("Missing completed Phone swipe for " + action.getString("label"));
                }
                gateMatch = awaitNativeClose(action, lastPhoneSwipeDown);
                scheduled = gateMatch.getLong("requested_uptime_ms");
            }
            waitUntil(scheduled);
            if ("tap".equals(action.getString("action"))) {
                if (firstTap || "window".equals(focusMode)) requireLauncher(action);
                else {
                    emit(new JSONObject().put("type", "focus").put("source", "scheduled")
                         .put("focus_mode", focusMode).put("guarded", false).put("focus_sampled", false).put("wait_ms", 0)
                         .put("label", action.getString("label")).put("uptime_ms", SystemClock.uptimeMillis()));
                }
                firstTap = false;
            }
            long actual = SystemClock.uptimeMillis();
            // A native gate replaces the fixed home gap. Move following events
            // with this tap so their dwell and cycle intervals remain intact.
            if (gateMatch == null) timelineShift += Math.max(0, actual - scheduled);
            else {
                timelineShift = actual - original;
                emit(gateMatch.put("uptime_ms", actual).put("tap_scheduled_uptime_ms", actual)
                     .put("wait_ms", actual - gateMatch.getLong("gate_begin_uptime_ms"))
                     .put("lateness_ms", Math.max(0, actual - scheduled)));
            }
            scheduled = original + timelineShift;
            emit(new JSONObject().put("type", "action_begin").put("index", index).put("label", action.getString("label"))
                 .put("uptime_ms", actual).put("original_uptime_ms", original).put("scheduled_uptime_ms", scheduled)
                 .put("timeline_shift_ms", timelineShift));
            if ("home-key".equals(action.getString("action"))) homeKey(action, scheduled);
            else gesture(action, scheduled);
            if ("swipe".equals(action.getString("action")) && "phone".equals(action.optString("target"))) {
                lastPhoneSwipeDown = lastTouchDownUptime;
                lastPhoneSwipeLabel = action.getString("label");
            }
            long completed = SystemClock.uptimeMillis();
            timelineShift += Math.max(0, completed - scheduled - action.getInt("duration_ms"));
            emit(new JSONObject().put("type", "action_end").put("index", index).put("label", action.getString("label"))
                 .put("uptime_ms", completed).put("timeline_shift_ms", timelineShift));
        }
        SystemClock.sleep(plan.getInt("settle_ms"));
        emit(new JSONObject().put("type", "complete").put("uptime_ms", SystemClock.uptimeMillis())
             .put("duration_ms", SystemClock.uptimeMillis() - begin).put("timeline_shift_ms", timelineShift));
    }

    public static void main(String[] args) throws Exception {
        if (args.length == 1 && "--help".equals(args[0])) {
            System.out.println("AvdGestureInput (--plan|--probe-plan) /data/local/tmp/plan.json");
            return;
        }
        try {
            if (args.length != 2 || !("--plan".equals(args[0]) || "--probe-plan".equals(args[0]))) {
                throw new IllegalArgumentException("Expected (--plan|--probe-plan) PATH");
            }
            StringBuilder input = new StringBuilder();
            try (BufferedReader reader = new BufferedReader(new FileReader(args[1]))) {
                String line;
                while ((line = reader.readLine()) != null) input.append(line);
            }
            JSONObject plan = new JSONObject(input.toString());
            AvdGestureInput injector = new AvdGestureInput(plan);
            if ("--probe-plan".equals(args[0])) {
                emit(injector.readFocus().put("type", "focus_probe").put("uptime_ms", SystemClock.uptimeMillis()));
            } else injector.run(plan);
        } catch (Exception error) {
            emit(new JSONObject().put("type", "error").put("uptime_ms", SystemClock.uptimeMillis())
                 .put("message", error.toString()));
            error.printStackTrace(System.err);
            System.exit(1);
        }
    }
}
