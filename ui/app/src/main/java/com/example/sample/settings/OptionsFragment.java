package com.example.sample.settings;

import android.os.Bundle;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.View;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.fragment.app.Fragment;
import androidx.lifecycle.ViewModelProvider;

import com.google.android.material.textfield.TextInputEditText;

public class OptionsFragment extends Fragment {
    public OptionsFragment() {
        super(R.layout.fragment_options);
    }

    private SharedViewModel vm;

    @Override
    public void onViewCreated(@NonNull View view, @Nullable Bundle savedInstanceState) {
        vm = new ViewModelProvider(requireActivity()).get(SharedViewModel.class);

        TextInputEditText editDelay = view.findViewById(R.id.editDelay);
        TextInputEditText editAddress = view.findViewById(R.id.editAddress);
        TextInputEditText editPort = view.findViewById(R.id.editPort);

        editDelay.setText(String.valueOf(vm.delayMicros));
        editAddress.setText(vm.listenAddress);
        editPort.setText(String.valueOf(vm.listenPort));

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
    }

    private int parseInt(Editable text, int fallback) {
        try {
            return Integer.parseInt(text != null ? text.toString().trim() : "");
        } catch (Throwable ignored) {
            return fallback;
        }
    }

    private abstract static class SimpleTextWatcher implements TextWatcher {
        @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
        @Override public void onTextChanged(CharSequence s, int start, int before, int count) {}
    }
}

