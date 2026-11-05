#include "ui/panel_rules.h"

namespace dirsize::ui {

std::wstring SelectionKey(std::uint32_t count, const std::wstring& path, bool inNavigationPane) {
    return std::to_wstring(count) + L"|" + path + (inNavigationPane ? L"|nav" : L"");
}

void PanelRules::Set(const std::wstring& folder) {
    if (folder == shown_) return;
    shown_ = folder;
    changed_ = true;
}

void PanelRules::Navigated(const std::wstring& folder, bool isFileSystemFolder, const std::wstring& selectionKey) {
    haveView_ = true;
    baseline_ = selectionKey;
    current_ = isFileSystemFolder ? folder : std::wstring();
    Set(current_);
}

void PanelRules::Selection(std::uint32_t count, const std::wstring& path, bool isFileSystemFolder,
                           bool inNavigationPane) {
    if (!haveView_) return;
    std::wstring key = SelectionKey(count, path, inNavigationPane);
    if (key == baseline_) return;
    baseline_ = key;
    if (count == 1 && isFileSystemFolder) Set(path);
    else if (count == 0 && inNavigationPane) Set(current_);
    else Set(std::wstring());
}

void PanelRules::ViewLost() {
    haveView_ = false;
    current_.clear();
    baseline_.clear();
    Set(std::wstring());
}

bool PanelRules::TakeChanged() {
    bool c = changed_;
    changed_ = false;
    return c;
}

}  // namespace dirsize::ui
