#include "flutter_window.h"

#include <commdlg.h>
#include <optional>
#include <vector>

#include "flutter/generated_plugin_registrant.h"
#include <flutter/standard_method_codec.h>
#include "utils.h"

namespace {

constexpr wchar_t kImportFileFilter[] =
    L"Config/Text Files (*.txt;*.sub;*.list;*.conf;*.json)\0*.txt;*.sub;*.list;*.conf;*.json\0"
    L"Text Files (*.txt)\0*.txt\0"
    L"All Files (*.*)\0*.*\0";

}  // namespace

FlutterWindow::FlutterWindow(const flutter::DartProject& project)
    : project_(project) {}

FlutterWindow::~FlutterWindow() {}

bool FlutterWindow::OnCreate() {
  if (!Win32Window::OnCreate()) {
    return false;
  }

  RECT frame = GetClientArea();

  // The size here must match the window dimensions to avoid unnecessary surface
  // creation / destruction in the startup path.
  flutter_controller_ = std::make_unique<flutter::FlutterViewController>(
      frame.right - frame.left, frame.bottom - frame.top, project_);
  // Ensure that basic setup of the controller was successful.
  if (!flutter_controller_->engine() || !flutter_controller_->view()) {
    return false;
  }
  RegisterPlugins(flutter_controller_->engine());
  RegisterNativeDialogChannel();
  SetChildContent(flutter_controller_->view()->GetNativeWindow());

  flutter_controller_->engine()->SetNextFrameCallback([&]() {
    this->Show();
  });

  // Flutter can complete the first frame before the "show window" callback is
  // registered. The following call ensures a frame is pending to ensure the
  // window is shown. It is a no-op if the first frame hasn't completed yet.
  flutter_controller_->ForceRedraw();

  return true;
}

void FlutterWindow::OnDestroy() {
  if (native_dialogs_channel_) {
    native_dialogs_channel_.reset();
  }
  if (flutter_controller_) {
    flutter_controller_ = nullptr;
  }

  Win32Window::OnDestroy();
}

void FlutterWindow::RegisterNativeDialogChannel() {
  native_dialogs_channel_ =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          flutter_controller_->engine()->messenger(), "hunter/native_dialogs",
          &flutter::StandardMethodCodec::GetInstance());

  native_dialogs_channel_->SetMethodCallHandler(
      [this](const flutter::MethodCall<flutter::EncodableValue>& call,
             std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
        if (call.method_name() != "pickImportTextFile") {
          result->NotImplemented();
          return;
        }

        std::string initial_directory;
        const flutter::EncodableValue* arguments = call.arguments();
        if (arguments != nullptr) {
          if (const auto* map = std::get_if<flutter::EncodableMap>(arguments)) {
            const auto it = map->find(flutter::EncodableValue("initialDirectory"));
            if (it != map->end()) {
              if (const auto* value = std::get_if<std::string>(&it->second)) {
                initial_directory = *value;
              }
            }
          }
        }

        const std::string selected = ShowImportTextFileDialog(initial_directory);
        if (selected.empty()) {
          result->Success();
          return;
        }
        result->Success(flutter::EncodableValue(selected));
      });
}

std::string FlutterWindow::ShowImportTextFileDialog(
    const std::string& initial_directory_utf8) {
  std::vector<wchar_t> file_path(32768, L'\0');
  std::wstring initial_directory = Utf16FromUtf8(initial_directory_utf8);

  OPENFILENAMEW dialog_config = {};
  dialog_config.lStructSize = sizeof(dialog_config);
  dialog_config.hwndOwner = GetHandle();
  dialog_config.lpstrFilter = kImportFileFilter;
  dialog_config.lpstrFile = file_path.data();
  dialog_config.nMaxFile = static_cast<DWORD>(file_path.size());
  dialog_config.lpstrTitle = L"Select config file to import";
  if (!initial_directory.empty()) {
    dialog_config.lpstrInitialDir = initial_directory.c_str();
  }
  dialog_config.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                        OFN_HIDEREADONLY | OFN_NOCHANGEDIR;

  if (!::GetOpenFileNameW(&dialog_config)) {
    return std::string();
  }
  return Utf8FromUtf16(file_path.data());
}

LRESULT
FlutterWindow::MessageHandler(HWND hwnd, UINT const message,
                              WPARAM const wparam,
                              LPARAM const lparam) noexcept {
  // Give Flutter, including plugins, an opportunity to handle window messages.
  if (flutter_controller_) {
    std::optional<LRESULT> result =
        flutter_controller_->HandleTopLevelWindowProc(hwnd, message, wparam,
                                                      lparam);
    if (result) {
      return *result;
    }
  }

  switch (message) {
    case WM_FONTCHANGE:
      flutter_controller_->engine()->ReloadSystemFonts();
      break;
  }

  return Win32Window::MessageHandler(hwnd, message, wparam, lparam);
}
