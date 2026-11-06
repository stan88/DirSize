#include "ui/verb_command.h"

namespace dirsize::ui {

std::wstring VerbCommandLine(const std::wstring& exe) { return L"\"" + exe + L"\" \"%1\""; }

std::wstring PathFromArgument(const std::wstring& arg) {
    // A path cannot contain a quotation mark, so a trailing one can only be
    // the escaped closing quote after a trailing backslash.
    if (!arg.empty() && arg.back() == L'"') return arg.substr(0, arg.size() - 1) + L"\\";
    return arg;
}

}  // namespace dirsize::ui
