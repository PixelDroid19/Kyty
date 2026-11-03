#ifndef LAUNCHER_INCLUDE_GAME_PATCH_MODS_H_
#define LAUNCHER_INCLUDE_GAME_PATCH_MODS_H_

#include <QString>
#include <QVariantList>

// The mods of a game patch file (docs/game-patches.md) as module-list entries: path holds the mod's name and
// selected its enabled flag. A file that cannot be read as a patch file has no entries.
[[nodiscard]] QVariantList ReadPatchMods(const QString& file);
// Writes the entries' selected flags to the file as the mods' enabled flags, and leaves the file untouched when none
// changed. Empty on success, otherwise what went wrong; the file is then unchanged.
[[nodiscard]] QString WritePatchMods(const QString& file, const QVariantList& mods);

#endif /* LAUNCHER_INCLUDE_GAME_PATCH_MODS_H_ */
