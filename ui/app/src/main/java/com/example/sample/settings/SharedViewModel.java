package com.example.sample.settings;

import androidx.lifecycle.ViewModel;

public class SharedViewModel extends ViewModel {
    public boolean showSystemApps = false;
    public String selectedPackage = "";
    public int delayMicros = 0;
    public String listenAddress = "0.0.0.0";
    public int listenPort = 8086;

    public void loadFromConfig(ConfigStore.Config cfg) {
        if (cfg == null) return;
        selectedPackage = cfg.packageName != null ? cfg.packageName : "";
        delayMicros = cfg.delayMicros;
        listenAddress = cfg.listenAddress != null ? cfg.listenAddress : "0.0.0.0";
        listenPort = cfg.listenPort;
    }
}

