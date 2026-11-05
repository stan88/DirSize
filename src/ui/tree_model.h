#pragma once
// The folder tree of the standalone window (design section 3), without
// window code so it is unit-tested:
//   - roots are drives ("C:\"); children are listed lazily, when a node is
//     expanded, and a later listing of the same folder keeps the children
//     that are still there (with their expansion);
//   - the visible rows, expand and collapse, keyboard moves;
//   - revealing a path the list navigated to: ancestors are expanded, and
//     the ones not listed yet are listed first (the reveal continues when
//     their listings arrive).
// Programmatic changes (Reveal, listings) never report a selection change;
// only user actions do, so the window navigates only on those.

#include <memory>
#include <string>
#include <vector>

namespace dirsize::ui {

enum class ChildState { NotLoaded, Loading, Loaded, Failed };

struct TreeNode {
    std::wstring name;   // "C:\" for a root, else the folder name
    std::wstring label;  // shown text; empty: the name
    TreeNode* parent = nullptr;
    std::vector<std::unique_ptr<TreeNode>> children;
    ChildState state = ChildState::NotLoaded;
    bool expanded = false;

    // Not listed yet (may have subfolders), being listed, or has children.
    bool HasExpander() const {
        return state == ChildState::NotLoaded || state == ChildState::Loading || !children.empty();
    }
};

struct TreeRoot {
    std::wstring path;   // "C:\"
    std::wstring label;  // empty: the path
};

struct TreeRow {
    TreeNode* node = nullptr;
    int depth = 0;
};

enum class TreeKey { Up, Down, Left, Right, Home, End, PageUp, PageDown };

// The result of a user action: whether the selection moved (the window then
// navigates) and the folders to list.
struct TreeAction {
    bool selectionChanged = false;
    std::vector<std::wstring> loads;
};

class TreeModel {
public:
    // Replaces the roots, keeping the nodes of roots still present. Returns
    // folders to list (a reveal waiting for its drive).
    std::vector<std::wstring> SetRoots(const std::vector<TreeRoot>& roots);
    // A listing of `path`: its subfolder names (any order). Returns folders
    // to list next (a reveal in progress).
    std::vector<std::wstring> SetChildren(const std::wstring& path, std::vector<std::wstring> names);
    // `path` could not be listed: no children, no expander.
    std::vector<std::wstring> SetFailed(const std::wstring& path);

    // Accepts "C:", "c:\x\" and other spellings; case-insensitive. Null for
    // paths outside the roots (network shares, missing drives).
    TreeNode* Find(const std::wstring& path) const;
    static std::wstring PathOf(const TreeNode* node);

    const std::vector<TreeRow>& Rows();  // rebuilt after changes
    int RowOf(const TreeNode* node);     // -1 when not visible
    TreeNode* Selected() const { return selected_; }
    // Set when a programmatic change moved the selection, so the window
    // scrolls it into view; cleared by the call.
    bool TakeScrollRequest();

    // ---- user actions (each one ends a reveal in progress) ----
    TreeAction Click(TreeNode* node);   // select
    TreeAction Toggle(TreeNode* node);  // expander, double-click
    TreeAction Key(TreeKey key, int pageRows);

    // ---- programmatic ----
    // Selects `path`, expanding its ancestors. A path outside the roots
    // clears the selection; a folder missing from its parent's listing (or
    // a parent that cannot be listed) stops at the deepest one found.
    // Returns folders to list.
    std::vector<std::wstring> Reveal(const std::wstring& path);
    // Lists `path` again (F5). Returns the folders to list.
    std::vector<std::wstring> Refresh(const std::wstring& path);

private:
    std::vector<std::wstring> ContinueReveal();
    std::vector<std::wstring> Expand(TreeNode* node);
    void Collapse(TreeNode* node, TreeAction& action);
    void SelectByUser(TreeNode* node, TreeAction& action);
    void Merge(TreeNode* parent, std::vector<std::wstring> names);
    void Changed() { rowsDirty_ = true; }

    std::vector<std::unique_ptr<TreeNode>> roots_;
    std::vector<TreeRow> rows_;
    bool rowsDirty_ = true;
    TreeNode* selected_ = nullptr;
    bool scrollRequest_ = false;
    bool revealing_ = false;
    std::wstring revealRoot_;
    std::vector<std::wstring> revealParts_;
};

// The listing rule of the tree loader (tree_loader.h), here so it is
// tested: a directory, not "." or "..", and not a name-surrogate reparse
// point (junction, symbolic link, mount point; the service's index rule).
// `reparseTag` is WIN32_FIND_DATAW::dwReserved0, meaningful only when
// FILE_ATTRIBUTE_REPARSE_POINT is set.
bool IsTreeFolder(unsigned long attributes, unsigned long reparseTag, const wchar_t* name);

// Splits "C:\a\b" into "C:\" and {"a", "b"}. False for paths that do not
// start with a drive letter.
bool SplitTreePath(const std::wstring& path, std::wstring& root, std::vector<std::wstring>& parts);

}  // namespace dirsize::ui
