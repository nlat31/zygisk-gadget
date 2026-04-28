package com.example.sample.settings;

import androidx.lifecycle.ViewModel;

public class SharedViewModel extends ViewModel {
    public boolean showSystemApps = false;
    public String selectedPackage = "";
    public int delayMicros = 0;
    public String interactionMode = ConfigStore.Config.MODE_LISTEN;
    public String listenAddress = "0.0.0.0";
    public int listenPort = 8086;
    public String scriptPath = ConfigStore.DEFAULT_SCRIPT_PATH;

    public void loadFromConfig(ConfigStore.Config cfg) {
        if (cfg == null) return;
        selectedPackage = cfg.packageName != null ? cfg.packageName : "";
        delayMicros = cfg.delayMicros;
        interactionMode = cfg.interactionMode != null ? cfg.interactionMode : ConfigStore.Config.MODE_LISTEN;
        listenAddress = cfg.listenAddress != null ? cfg.listenAddress : "0.0.0.0";
        listenPort = cfg.listenPort;
        scriptPath = cfg.scriptPath != null ? cfg.scriptPath : ConfigStore.DEFAULT_SCRIPT_PATH;
    }
}

