#include "pch.h"
#include "draw_gui_for_open_dat_file.h"
#include "GuiGlobalConstants.h"
#include <filesystem>

void draw_gui_for_open_dat_file()
{
    constexpr auto window_flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground;
    constexpr auto window_name = "Select your Gw.dat file";
    ImGui::SetNextWindowSize(ImVec2(360, 200));
    ImGui::Begin(window_name, nullptr, window_flags);

    ImVec2 window_size = ImGui::GetWindowSize();
    ImVec2 screen_size = ImGui::GetIO().DisplaySize;
    ImVec2 window_pos = (screen_size - window_size) * 0.5f;
    ImGui::SetWindowPos(window_pos);

    ImVec2 button_size = ImVec2(200, 40);
    float x = (window_size.x - button_size.x) / 2.0f;
    float y = (window_size.y - button_size.y) / 2.0f;
    ImGui::SetCursorPos(ImVec2(x, y - 20.0f));

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
    const char* hint = "Use File > Load .dat File  or click below";
    float text_width = ImGui::CalcTextSize(hint).x;
    ImGui::SetCursorPosX((window_size.x - text_width) / 2.0f);
    ImGui::TextUnformatted(hint);
    ImGui::PopStyleColor();

    ImGui::SetCursorPos(ImVec2(x, y + 10.0f));

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
    if (ImGui::Button("Select a \"Gw.dat\" File", button_size))
    {
        std::string initial_filepath = ".";

        if (!GuiGlobalConstants::saved_gw_dat_path.empty()) {
            auto parentDir = std::filesystem::path(GuiGlobalConstants::saved_gw_dat_path).parent_path();
            if (std::filesystem::exists(parentDir))
                initial_filepath = parentDir.string();
        }

        if (!std::filesystem::exists(initial_filepath) || !std::filesystem::is_directory(initial_filepath)) {
            const auto filepath_existing = load_last_filepath("dat_browser_last_filepath.txt");
            if (filepath_existing.has_value())
                initial_filepath = filepath_existing.value().parent_path().string();
        }

        if (!std::filesystem::exists(initial_filepath) || !std::filesystem::is_directory(initial_filepath)) {
            const auto filepath_curr_dir = get_executable_directory();
            if (filepath_curr_dir.has_value())
                initial_filepath = filepath_curr_dir.value().string();
        }

        ImGuiFileDialog::Instance()->OpenDialog("ChooseGwDatKey", "Select Gw.dat", ".dat",
                                                initial_filepath + "\\.");
    }
    ImGui::PopStyleColor(3);

    ImGui::End();
}
