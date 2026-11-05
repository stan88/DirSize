#include "ui/tree_model.h"

#include <windows.h>

#include <algorithm>

#include "ui/list_model.h"

namespace dirsize::ui {

namespace {

// File names compare case-insensitively (as NTFS does by default).
bool SameName(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

TreeNode* FindChild(const TreeNode* parent, const std::wstring& name) {
    for (const auto& c : parent->children) {
        if (SameName(c->name, name)) return c.get();
    }
    return nullptr;
}

bool IsInside(const TreeNode* node, const TreeNode* ancestor) {
    for (const TreeNode* n = node; n; n = n->parent) {
        if (n == ancestor) return true;
    }
    return false;
}

void AddRows(TreeNode* node, int depth, std::vector<TreeRow>& rows) {
    rows.push_back({node, depth});
    if (!node->expanded) return;
    for (const auto& c : node->children) AddRows(c.get(), depth + 1, rows);
}

}  // namespace

bool IsTreeFolder(unsigned long attributes, unsigned long reparseTag, const wchar_t* name) {
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    if (name[0] == L'.' && (name[1] == 0 || (name[1] == L'.' && name[2] == 0))) return false;
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(reparseTag)) return false;
    return true;
}

bool SplitTreePath(const std::wstring& path, std::wstring& root, std::vector<std::wstring>& parts) {
    parts.clear();
    if (path.size() < 2 || path[1] != L':') return false;
    wchar_t letter = path[0];
    if (letter >= L'a' && letter <= L'z') letter = static_cast<wchar_t>(letter - L'a' + L'A');
    if (letter < L'A' || letter > L'Z') return false;
    if (path.size() > 2 && path[2] != L'\\') return false;  // "C:x" (relative to C:'s current folder)
    root = std::wstring(1, letter) + L":\\";
    size_t i = 2;
    while (i < path.size()) {
        while (i < path.size() && path[i] == L'\\') ++i;
        size_t end = path.find(L'\\', i);
        if (end == std::wstring::npos) end = path.size();
        if (end > i) parts.push_back(path.substr(i, end - i));
        i = end;
    }
    return true;
}

std::vector<std::wstring> TreeModel::SetRoots(const std::vector<TreeRoot>& roots) {
    std::vector<std::unique_ptr<TreeNode>> next;
    for (const TreeRoot& r : roots) {
        std::wstring root;
        std::vector<std::wstring> parts;
        if (!SplitTreePath(r.path, root, parts) || !parts.empty()) continue;
        std::unique_ptr<TreeNode> node;
        for (auto& old : roots_) {
            if (old && old->name == root) node = std::move(old);
        }
        if (!node) {
            node = std::make_unique<TreeNode>();
            node->name = root;
        }
        node->label = r.label;
        next.push_back(std::move(node));
    }
    for (const auto& old : roots_) {
        if (old && selected_ && IsInside(selected_, old.get())) selected_ = nullptr;
    }
    roots_ = std::move(next);
    Changed();
    return ContinueReveal();
}

void TreeModel::Merge(TreeNode* parent, std::vector<std::wstring> names) {
    // Stable: of two names equal but for case, the first listed is kept.
    std::stable_sort(names.begin(), names.end(),
              [](const std::wstring& a, const std::wstring& b) { return CompareNames(a, b) < 0; });
    std::vector<std::unique_ptr<TreeNode>> next;
    for (std::wstring& name : names) {
        if (!next.empty() && SameName(next.back()->name, name)) continue;
        std::unique_ptr<TreeNode> node;
        for (auto& old : parent->children) {
            if (old && SameName(old->name, name)) {
                node = std::move(old);
                break;
            }
        }
        if (!node) {
            node = std::make_unique<TreeNode>();
            node->parent = parent;
        }
        node->name = std::move(name);  // the listing's spelling wins
        next.push_back(std::move(node));
    }
    for (const auto& old : parent->children) {
        if (old && selected_ && IsInside(selected_, old.get())) {
            selected_ = parent;  // the selected folder is gone
            scrollRequest_ = true;
        }
    }
    parent->children = std::move(next);
}

std::vector<std::wstring> TreeModel::SetChildren(const std::wstring& path, std::vector<std::wstring> names) {
    TreeNode* node = Find(path);
    if (!node) return {};
    Merge(node, std::move(names));
    node->state = ChildState::Loaded;
    if (node->children.empty()) node->expanded = false;
    Changed();
    return ContinueReveal();
}

std::vector<std::wstring> TreeModel::SetFailed(const std::wstring& path) {
    TreeNode* node = Find(path);
    if (!node) return {};
    Merge(node, {});
    node->state = ChildState::Failed;
    node->expanded = false;
    Changed();
    return ContinueReveal();
}

TreeNode* TreeModel::Find(const std::wstring& path) const {
    std::wstring root;
    std::vector<std::wstring> parts;
    if (!SplitTreePath(path, root, parts)) return nullptr;
    TreeNode* node = nullptr;
    for (const auto& r : roots_) {
        if (r->name == root) node = r.get();
    }
    for (const std::wstring& part : parts) {
        if (!node) break;
        node = FindChild(node, part);
    }
    return node;
}

std::wstring TreeModel::PathOf(const TreeNode* node) {
    if (!node) return L"";
    if (!node->parent) return node->name;
    std::wstring parent = PathOf(node->parent);
    if (parent.back() != L'\\') parent += L'\\';
    return parent + node->name;
}

const std::vector<TreeRow>& TreeModel::Rows() {
    if (rowsDirty_) {
        rows_.clear();
        for (const auto& r : roots_) AddRows(r.get(), 0, rows_);
        rowsDirty_ = false;
    }
    return rows_;
}

int TreeModel::RowOf(const TreeNode* node) {
    const auto& rows = Rows();
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].node == node) return static_cast<int>(i);
    }
    return -1;
}

bool TreeModel::TakeScrollRequest() {
    bool r = scrollRequest_;
    scrollRequest_ = false;
    return r;
}

std::vector<std::wstring> TreeModel::Expand(TreeNode* node) {
    if (!node || !node->HasExpander()) return {};
    node->expanded = true;
    Changed();
    if (node->state != ChildState::NotLoaded) return {};
    node->state = ChildState::Loading;
    return {PathOf(node)};
}

void TreeModel::Collapse(TreeNode* node, TreeAction& action) {
    if (!node || !node->expanded) return;
    node->expanded = false;
    Changed();
    if (selected_ && selected_ != node && IsInside(selected_, node)) {
        selected_ = node;  // as Explorer: the selection moves up to the collapsed folder
        action.selectionChanged = true;
    }
}

void TreeModel::SelectByUser(TreeNode* node, TreeAction& action) {
    if (!node || node == selected_) return;
    selected_ = node;
    action.selectionChanged = true;
}

TreeAction TreeModel::Click(TreeNode* node) {
    revealing_ = false;
    TreeAction action;
    SelectByUser(node, action);
    return action;
}

TreeAction TreeModel::Toggle(TreeNode* node) {
    revealing_ = false;
    TreeAction action;
    if (!node) return action;
    if (node->expanded) Collapse(node, action);
    else action.loads = Expand(node);
    return action;
}

TreeAction TreeModel::Key(TreeKey key, int pageRows) {
    revealing_ = false;
    TreeAction action;
    const auto& rows = Rows();
    if (rows.empty()) return action;
    const int count = static_cast<int>(rows.size());
    const int current = RowOf(selected_);
    const int page = std::max(1, pageRows);
    int target = current;
    switch (key) {
        case TreeKey::Up: target = current < 0 ? 0 : current - 1; break;
        case TreeKey::Down: target = current + 1; break;
        case TreeKey::Home: target = 0; break;
        case TreeKey::End: target = count - 1; break;
        case TreeKey::PageUp: target = std::max(0, current) - page; break;
        case TreeKey::PageDown: target = std::max(0, current) + page; break;
        case TreeKey::Left:
            if (!selected_) return action;
            if (selected_->expanded) Collapse(selected_, action);
            else SelectByUser(selected_->parent, action);
            return action;
        case TreeKey::Right:
            if (!selected_) return action;
            if (!selected_->expanded) action.loads = Expand(selected_);
            else if (!selected_->children.empty()) SelectByUser(selected_->children.front().get(), action);
            return action;
    }
    target = std::clamp(target, 0, count - 1);
    SelectByUser(rows[target].node, action);
    return action;
}

std::vector<std::wstring> TreeModel::Reveal(const std::wstring& path) {
    revealing_ = SplitTreePath(path, revealRoot_, revealParts_);
    if (!revealing_) {
        selected_ = nullptr;  // outside the drives (a network share)
        return {};
    }
    return ContinueReveal();
}

std::vector<std::wstring> TreeModel::ContinueReveal() {
    if (!revealing_) return {};
    TreeNode* node = nullptr;
    for (const auto& r : roots_) {
        if (r->name == revealRoot_) node = r.get();
    }
    if (!node) {
        // The drive is not in the list (yet): nothing to select. The reveal
        // stays active and continues when SetRoots brings the drive; a later
        // Reveal or user action replaces it.
        selected_ = nullptr;
        return {};
    }
    auto stopAt = [&](TreeNode* n) {
        if (selected_ != n) scrollRequest_ = true;
        selected_ = n;
    };
    for (const std::wstring& part : revealParts_) {
        if (node->state == ChildState::NotLoaded) {
            node->state = ChildState::Loading;
            node->expanded = true;
            Changed();
            stopAt(node);
            return {PathOf(node)};
        }
        if (node->state == ChildState::Loading) {
            if (!node->expanded) Changed();
            node->expanded = true;
            stopAt(node);
            return {};  // wait for its listing
        }
        TreeNode* child = FindChild(node, part);
        if (!child) {  // listing failed, or the folder is not in it
            revealing_ = false;
            stopAt(node);
            return {};
        }
        if (!node->expanded) Changed();
        node->expanded = true;
        node = child;
    }
    revealing_ = false;
    stopAt(node);
    return {};
}

std::vector<std::wstring> TreeModel::Refresh(const std::wstring& path) {
    TreeNode* node = Find(path);
    if (!node || node->state == ChildState::Loading) return {};
    if (node->state == ChildState::NotLoaded && !node->expanded) return {};  // listed when expanded
    node->state = ChildState::Loading;
    Changed();
    return {PathOf(node)};
}

}  // namespace dirsize::ui
