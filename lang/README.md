# Translations / 翻譯

[English](#english) · [繁體中文](#繁體中文)

## English

Each file here is one interface language of `dirsize.exe`. The build turns
them into string tables inside the executable (`cmake/gen_lang.cmake`), so
DirSize stays a single file.

### Adding a language

1. Copy `en-US.json` to `<locale>.json`, for example `ja-JP.json`.
2. Set the header fields:

   | Field | Meaning | Example |
   |---|---|---|
   | `locale` | Locale name; must match the file name | `"ja-JP"` |
   | `language`, `sublanguage` | The `LANG_` and `SUBLANG_` names from the Windows SDK header `winnt.h` for that locale | `"LANG_JAPANESE"`, `"SUBLANG_JAPANESE_JAPAN"` |
   | `matches` | Windows display languages this file serves. An entry matches the same name or any name that starts with it plus `-` (`"ja"` matches `ja-JP`). | `["ja"]` |

3. Translate the values under `strings`. Leave the keys (`IDS_...`) as they are.
4. Build (see the main README). `ui-tests` checks that `language`/`sublanguage`
   give the same language ID Windows has for `locale`, and that every string
   loads.

### Rules

- `en-US.json` is the base and must contain every ID in `src/ui/string_ids.h`.
- A missing string in another language gets the English text, with a build
  warning. Windows does not fall back on its own for a single missing string.
- `%1`, `%2`… are values filled in by the program. Keep the same placeholders
  as the English text; you may change their order. The build fails otherwise.
- `\t` in a menu item separates the shortcut key (`"Refresh\tF5"`).
- Save as UTF-8. A byte order mark is fine.

### How the language is chosen

DirSize goes through the user's Windows display languages in order and takes
the first one that a file serves (its `locale` or `matches`). If none does, it
uses English. A choice in the "⋯" menu → Language overrides this and takes
effect at once (an open Explorer panel switches when it is opened again).

## 繁體中文

這裡每個檔案是 `dirsize.exe` 的一種介面語言。建置時會轉成字串表編進執行檔（`cmake/gen_lang.cmake`），所以 DirSize 仍然只有一個檔案。

### 新增語言

1. 把 `en-US.json` 複製成 `<語言>.json`，例如 `ja-JP.json`。
2. 填寫開頭的欄位：

   | 欄位 | 意義 | 範例 |
   |---|---|---|
   | `locale` | 語言名稱，要和檔名相同 | `"ja-JP"` |
   | `language`、`sublanguage` | Windows SDK 標頭檔 `winnt.h` 裡，這個語言的 `LANG_` 和 `SUBLANG_` 名稱 | `"LANG_JAPANESE"`、`"SUBLANG_JAPANESE_JAPAN"` |
   | `matches` | 這個檔案適用的 Windows 顯示語言。名稱相同，或是以它加上 `-` 開頭的名稱都算（`"ja"` 對得到 `ja-JP`） | `["ja"]` |

3. 翻譯 `strings` 底下的值，名稱（`IDS_...`）不要改。
4. 建置（見主 README）。`ui-tests` 會檢查 `language`／`sublanguage` 和 Windows 對 `locale` 的語言代碼是否一致，以及每個字串都載入得到。

### 規則

- `en-US.json` 是基準，必須包含 `src/ui/string_ids.h` 裡的每一個 ID。
- 其他語言缺少的字串，會用英文代替，建置時會出現警告。Windows 不會針對單一個缺少的字串改用其他語言。
- `%1`、`%2`… 是程式填入的值。要和英文的占位符號相同，順序可以調整；不同的話建置會失敗。
- 選單項目裡的 `\t` 用來分隔快速鍵（`"重新整理\tF5"`）。
- 用 UTF-8 存檔，有沒有 BOM 都可以。

### 怎麼選語言

DirSize 依序看使用者的 Windows 顯示語言，用第一個有檔案適用的語言（`locale` 或 `matches`）；都沒有就用英文。在「⋯」選單 → 語言裡手動選的語言優先，選了立刻生效（已經開著的檔案總管面板要重新開啟才會切換）。
