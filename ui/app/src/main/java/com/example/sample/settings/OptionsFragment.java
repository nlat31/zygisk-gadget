package com.example.sample.settings;

import android.app.AlertDialog;
import android.os.Bundle;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.View;
import android.widget.RadioGroup;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.fragment.app.Fragment;
import androidx.lifecycle.ViewModelProvider;

import com.google.android.material.textfield.TextInputEditText;

import java.util.ArrayList;
import java.util.List;

public class OptionsFragment extends Fragment {
    private static final String DEFAULT_SCRIPT_DIR = "/data/local/tmp";

    public OptionsFragment() {
        super(R.layout.fragment_options);
    }

    private SharedViewModel vm;
    private View listenOptions;
    private View scriptOptions;
    private TextInputEditText editScriptPath;

    @Override
    public void onViewCreated(@NonNull View view, @Nullable Bundle savedInstanceState) {
        vm = new ViewModelProvider(requireActivity()).get(SharedViewModel.class);

        TextInputEditText editDelay = view.findViewById(R.id.editDelay);
        TextInputEditText editAddress = view.findViewById(R.id.editAddress);
        TextInputEditText editPort = view.findViewById(R.id.editPort);
        editScriptPath = view.findViewById(R.id.editScriptPath);
        RadioGroup groupInteractionMode = view.findViewById(R.id.groupInteractionMode);
        listenOptions = view.findViewById(R.id.listenOptions);
        scriptOptions = view.findViewById(R.id.scriptOptions);
        View btnBrowseScript = view.findViewById(R.id.btnBrowseScript);

        editDelay.setText(String.valueOf(vm.delayMicros));
        editAddress.setText(vm.listenAddress);
        editPort.setText(String.valueOf(vm.listenPort));
        editScriptPath.setText(vm.scriptPath);
        groupInteractionMode.check(ConfigStore.Config.MODE_SCRIPT.equals(vm.interactionMode)
            ? R.id.radioScript
            : R.id.radioListen);
        updateModeVisibility();

        editDelay.addTextChangedListener(new SimpleTextWatcher() {
            @Override
            public void afterTextChanged(Editable s) {
                vm.delayMicros = parseInt(s, 0);
            }
        });
        editAddress.addTextChangedListener(new SimpleTextWatcher() {
            @Override
            public void afterTextChanged(Editable s) {
                String value = s != null ? s.toString().trim() : "";
                vm.listenAddress = value.isEmpty() ? "0.0.0.0" : value;
            }
        });
        editPort.addTextChangedListener(new SimpleTextWatcher() {
            @Override
            public void afterTextChanged(Editable s) {
                vm.listenPort = parseInt(s, 8086);
            }
        });
        editScriptPath.addTextChangedListener(new SimpleTextWatcher() {
            @Override
            public void afterTextChanged(Editable s) {
                String value = s != null ? s.toString().trim() : "";
                vm.scriptPath = value.isEmpty() ? ConfigStore.DEFAULT_SCRIPT_PATH : value;
            }
        });

        groupInteractionMode.setOnCheckedChangeListener((group, checkedId) -> {
            vm.interactionMode = checkedId == R.id.radioScript
                ? ConfigStore.Config.MODE_SCRIPT
                : ConfigStore.Config.MODE_LISTEN;
            updateModeVisibility();
        });
        btnBrowseScript.setOnClickListener(v -> showFileBrowser(initialBrowseDir()));
    }

    private int parseInt(Editable text, int fallback) {
        try {
            return Integer.parseInt(text != null ? text.toString().trim() : "");
        } catch (Throwable ignored) {
            return fallback;
        }
    }

    private void updateModeVisibility() {
        boolean scriptMode = ConfigStore.Config.MODE_SCRIPT.equals(vm.interactionMode);
        listenOptions.setVisibility(scriptMode ? View.GONE : View.VISIBLE);
        scriptOptions.setVisibility(scriptMode ? View.VISIBLE : View.GONE);
    }

    private String initialBrowseDir() {
        String path = vm.scriptPath != null ? vm.scriptPath.trim() : "";
        int slash = path.lastIndexOf('/');
        if (slash > 0) {
            return path.substring(0, slash);
        }
        return DEFAULT_SCRIPT_DIR;
    }

    private void showFileBrowser(String directory) {
        String dir = normalizeDir(directory);
        RootShell.Result result = RootShell.exec("cd " + shellQuote(dir) + " 2>/dev/null && ls -1Ap");
        if (result.code != 0) {
            Toast.makeText(requireContext(), "Cannot open " + dir, Toast.LENGTH_LONG).show();
            return;
        }

        List<String> labels = new ArrayList<>();
        List<String> paths = new ArrayList<>();
        if (!"/".equals(dir)) {
            labels.add("../");
            paths.add(parentDir(dir));
        }

        String out = result.out != null ? result.out : "";
        for (String line : out.split("\\n")) {
            String name = line.trim();
            if (name.isEmpty()) continue;
            labels.add(name);
            paths.add("/".equals(dir) ? "/" + stripMarker(name) : dir + "/" + stripMarker(name));
        }

        new AlertDialog.Builder(requireContext())
            .setTitle(getString(R.string.select_script) + ": " + dir)
            .setItems(labels.toArray(new String[0]), (dialog, which) -> {
                String label = labels.get(which);
                String path = paths.get(which);
                if (label.endsWith("/")) {
                    showFileBrowser(path);
                } else {
                    vm.scriptPath = path;
                    editScriptPath.setText(path);
                }
            })
            .show();
    }

    private String normalizeDir(String dir) {
        String value = dir != null && !dir.trim().isEmpty() ? dir.trim() : DEFAULT_SCRIPT_DIR;
        while (value.length() > 1 && value.endsWith("/")) {
            value = value.substring(0, value.length() - 1);
        }
        return value;
    }

    private String parentDir(String dir) {
        String value = normalizeDir(dir);
        int slash = value.lastIndexOf('/');
        return slash <= 0 ? "/" : value.substring(0, slash);
    }

    private String stripMarker(String name) {
        if (name.endsWith("/") || name.endsWith("*") || name.endsWith("@") || name.endsWith("|") || name.endsWith("=")) {
            return name.substring(0, name.length() - 1);
        }
        return name;
    }

    private String shellQuote(String value) {
        return "'" + value.replace("'", "'\\''") + "'";
    }

    private abstract static class SimpleTextWatcher implements TextWatcher {
        @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
        @Override public void onTextChanged(CharSequence s, int start, int before, int count) {}
    }
}

