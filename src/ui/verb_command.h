#pragma once
// The command line of the Explorer verb "Open with DirSize" (shell_verb.h)
// and how the program reads the folder back from it. Pure, so tested.

#include <string>

namespace dirsize::ui {

// "\"<exe>\" \"%1\"". The verb documentation (Verbs and File Associations)
// asks for quotation marks around "%1".
std::wstring VerbCommandLine(const std::wstring& exe);

// "\"<exe>\" --panel": the command that starts the Explorer panel at sign-in
// (shell_verb.h, SetPanelAutostart).
std::wstring PanelCommandLine(const std::wstring& exe);

// The folder argument as the program receives it. For a drive Explorer
// passes "C:\", so the command line holds "C:\"" -- an odd number of
// backslashes before a quotation mark, which the documented parsing rules
// (CommandLineToArgvW) turn into a literal quotation mark: the argument
// arrives as C:" . This turns that back into C:\ ; other arguments are
// returned unchanged.
std::wstring PathFromArgument(const std::wstring& arg);

}  // namespace dirsize::ui
