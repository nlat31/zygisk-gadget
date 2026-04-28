package com.example.sample.settings;

import android.util.Base64;

import org.json.JSONObject;

import java.nio.charset.StandardCharsets;

public final class ConfigStore {
    private ConfigStore() {}

    public static final String DEFAULT_SCRIPT_PATH = "/data/local/tmp/hook.js";

    private static final String MOD_ID = "zygisk_gadget";
    private static final String MODULE_DIR = "/data/adb/modules/" + MOD_ID;
    private static final String CONFIG_PATH = MODULE_DIR + "/config";
    private static final String GADGET_CONFIG_PATH = MODULE_DIR + "/frida-gadget.config";

    public static final class Config {
        public static final String MODE_LISTEN = "listen";
        public static final String MODE_SCRIPT = "script";

        public final String packageName;
        public final int delayMicros;
        public final String interactionMode;
        public final String listenAddress;
        public final int listenPort;
        public final String scriptPath;

        public Config(String packageName, int delayMicros, String interactionMode, String listenAddress, int listenPort, String scriptPath) {
            this.packageName = packageName != null ? packageName : "";
            this.delayMicros = Math.max(delayMicros, 0);
            this.interactionMode = MODE_SCRIPT.equals(interactionMode) ? MODE_SCRIPT : MODE_LISTEN;
            this.listenAddress = listenAddress != null && !listenAddress.trim().isEmpty()
                ? listenAddress.trim()
                : "0.0.0.0";
            this.listenPort = listenPort > 0 ? listenPort : 8086;
            this.scriptPath = scriptPath != null && !scriptPath.trim().isEmpty()
                ? scriptPath.trim()
                : DEFAULT_SCRIPT_PATH;
        }

        public Config(String packageName, int delayMicros, String listenAddress, int listenPort) {
            this(packageName, delayMicros, MODE_LISTEN, listenAddress, listenPort, DEFAULT_SCRIPT_PATH);
        }
    }

    public static Config readConfigOrNull() {
        try {
            JSONObject moduleConfig = readJson(CONFIG_PATH);
            if (moduleConfig == null) return null;

            JSONObject pkg = moduleConfig.optJSONObject("package");
            String packageName = pkg != null ? pkg.optString("name", "") : "";
            int delay = pkg != null ? pkg.optInt("delay", 0) : 0;

            JSONObject gadgetConfig = readJson(GADGET_CONFIG_PATH);
            JSONObject interaction = gadgetConfig != null
                ? gadgetConfig.optJSONObject("interaction")
                : null;
            String mode = interaction != null ? interaction.optString("type", Config.MODE_LISTEN) : Config.MODE_LISTEN;
            String address = interaction != null ? interaction.optString("address", "0.0.0.0") : "0.0.0.0";
            int port = interaction != null ? interaction.optInt("port", 8086) : 8086;
            String scriptPath = interaction != null ? interaction.optString("path", DEFAULT_SCRIPT_PATH) : DEFAULT_SCRIPT_PATH;

            return new Config(packageName, delay, mode, address, port, scriptPath);
        } catch (Throwable t) {
            return null;
        }
    }

    public static RootShell.Result writeConfig(Config cfg) {
        try {
            Config safe = cfg != null ? cfg : new Config("", 0, Config.MODE_LISTEN, "0.0.0.0", 8086, DEFAULT_SCRIPT_PATH);

            JSONObject packageMode = new JSONObject();
            packageMode.put("config", true);

            JSONObject packageObj = new JSONObject();
            packageObj.put("name", safe.packageName);
            packageObj.put("delay", safe.delayMicros);
            packageObj.put("mode", packageMode);

            JSONObject moduleConfig = new JSONObject();
            moduleConfig.put("package", packageObj);

            JSONObject interaction = new JSONObject();
            interaction.put("type", safe.interactionMode);
            if (Config.MODE_SCRIPT.equals(safe.interactionMode)) {
                interaction.put("path", safe.scriptPath);
            } else {
                interaction.put("address", safe.listenAddress);
                interaction.put("port", safe.listenPort);
                interaction.put("on_port_conflict", "fail");
                interaction.put("on_load", "wait");
            }

            JSONObject gadgetConfig = new JSONObject();
            gadgetConfig.put("interaction", interaction);

            String moduleB64 = encodeJson(moduleConfig);
            String gadgetB64 = encodeJson(gadgetConfig);

            String cmd =
                "mkdir -p '" + MODULE_DIR + "' && " +
                "umask 022 && " +
                "printf '%s' '" + moduleB64 + "' | base64 -d > '" + CONFIG_PATH + "' && " +
                "printf '%s' '" + gadgetB64 + "' | base64 -d > '" + GADGET_CONFIG_PATH + "' && " +
                "chmod 0644 '" + CONFIG_PATH + "' '" + GADGET_CONFIG_PATH + "'";

            return RootShell.exec(cmd);
        } catch (Throwable t) {
            return new RootShell.Result(1, "", String.valueOf(t));
        }
    }

    private static JSONObject readJson(String path) throws Exception {
        RootShell.Result r = RootShell.exec("cat '" + path + "' 2>/dev/null");
        if (r.code != 0) return null;
        String txt = r.out != null ? r.out.trim() : "";
        if (txt.isEmpty()) return null;
        return new JSONObject(txt);
    }

    private static String encodeJson(JSONObject obj) throws Exception {
        String json = obj.toString(2);
        return Base64.encodeToString(json.getBytes(StandardCharsets.UTF_8), Base64.NO_WRAP);
    }
}

