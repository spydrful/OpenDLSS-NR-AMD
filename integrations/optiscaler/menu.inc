        if (DlssNr::AmdBridge::HasFiles() &&
            DlssNr::Backend::ActiveKindFromConfig() == DlssNr::Backend::Kind::Mochizuki)
        {
            HGap(0.12f);
            ImGui::TextDisabled("OpenDLSS-NR AMD (development)");
            ImGui::TextWrapped("One pass at the full render resolution before FSR.");
            const auto slider = [](const char* label, CustomOptional<float>& option, float lo, float hi)
            {
                float value = option.value_or_default();
                if (ImGui::SliderFloat(label, &value, lo, hi, "%.2f")) option = value;
            };
            slider("Effect strength", config->MochizukiIntensity, 0.0f, 2.0f);
            slider("Colour strength", config->MochizukiColourStrength, 0.0f, 4.0f);
            slider("Highlight guard", config->MochizukiMaxRatio, 1.0f, 8.0f);
            ImGui::SeparatorText("Model controls");
            int style = int(std::min(config->MochizukiStyle.value_or_default(), 2u));
            if (ImGui::Combo("Style", &style, "Standard\0Natural\0Cinematic\0")) config->MochizukiStyle = uint32_t(style);
            slider("Local tone", config->MochizukiLocalTone, 0.0f, 2.0f);
            slider("Local structure", config->MochizukiLocalStructure, 0.0f, 2.0f);
            slider("Skin structure", config->MochizukiSkinStructure, -1.0f, 2.0f);
            slider("White point", config->MochizukiWhitePoint, 0.05f, 8.0f);
            bool mask = config->MochizukiAutoMask.value_or_default();
            if (ImGui::Checkbox("Automatic skin mask", &mask)) config->MochizukiAutoMask = mask;
            ImGui::SeparatorText("Temporal history");
            bool temporal = config->MochizukiTemporal.has_value() ? config->MochizukiTemporal.value() : config->LmxxfTemporal.value_or_default();
            if (ImGui::Checkbox("Use motion and history", &temporal)) config->MochizukiTemporal = temporal;
            slider("History strength", config->MochizukiHistoryStrength, 0.0f, 1.0f);
            ImGui::SeparatorText("Status");
            ImGui::TextWrapped("%s", DlssNr::AmdBridge::Status().c_str());
            if (const auto status = DlssNr::AmdBridge::RuntimeStatus(); !status.empty()) ImGui::TextWrapped("%s", status.c_str());
            if (MochizukiNrInfo info {}; DlssNr::Backend::LmxxfBackend::MochizukiInfo(info))
            {
                ImGui::Text("Render input: %u x %u", info.frame_w, info.frame_h);
                if (info.gpu_ms_median > 0) ImGui::Text("NR + bridge: %.2f ms median, %.2f ms P95", info.gpu_ms_median, info.gpu_ms_p95);
            }
            ImGui::TextWrapped("GPU timing covers NR and its bridge. Measure complete game frame times for FPS.");
            return;
        }
