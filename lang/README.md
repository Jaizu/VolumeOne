# Translations

Each language is a plain text file in this folder, named with its language code: `en.lang`, `es.lang`, ... VolumeOne picks the file that matches the Windows display language and uses English for anything missing.

## Adding a language

1. Copy `en.lang` to `<code>.lang` (for example `fr.lang`) and translate the text after each `=`. Keep the keys, `{0}` and `\n` as they are. Save as UTF-8.
2. Add a line for it in `res/app.rc`, next to the others:
   ```
   FR LANG "../lang/fr.lang"
   ```
3. Build with `build.cmd`.

To suggest a language or fix a translation without building anything, open an issue or a pull request with the `.lang` file.
