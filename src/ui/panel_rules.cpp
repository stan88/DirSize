#include "ui/panel_rules.h"

namespace dirsize::ui {

std::wstring SelectionKey(std::uint32_t count, const std::wstring& path) {
    return std::to_wstring(count) + L"|" + path;
}

void PanelRules::Set(const std::wstring& folder) {
    if (folder == shown_) return;
    shown_ = folder;
    changed_ = true;
}

void PanelRules::Navigated(const std::wstring& folder, bool isFileSystemFolder, const std::wstring& selectionKey) {
    haveView_ = true;
    baseline_ = selectionKey;
    Set(isFileSystemFolder ? folder : std::wstring());
}

void PanelRules::Selection(std::uint32_t count, const std::wstring& path, bool isFileSystemFolder) {
    if (!haveView_) return;
    std::wstring key = SelectionKey(count, path);
    if (key == baseline_) return;
    baseline_ = key;
    Set(count == 1 && isFileSystemFolder ? path : std::wstring());
}

void PanelRules::ViewLost() {
    haveView_ = false;
    baseline_.clear();
    Set(std::wstring());
}

bool PanelRules::TakeChanged() {
    bool c = changed_;
    changed_ = false;
    return c;
}

}  // namespace dirsize::ui
