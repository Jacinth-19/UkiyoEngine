#include "asset_ops.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSet>
#include <QStandardPaths>

#include <functional>

namespace assetops {
namespace {

// Folders that never hold project content, skipped when looking for references.
bool skipped(const QString& path) {
    return path.contains(QLatin1String("/build/")) || path.contains(QLatin1String("/third_party/")) ||
           path.contains(QLatin1String("/.git/"));
}

// Every reference file under root, as absolute paths.
QStringList referenceFiles(const QString& root) {
    QStringList out;
    QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString p = it.next();
        if (!skipped(p) && isReferenceFile(p)) out << p;
    }
    return out;
}

void collectStrings(const QJsonValue& v, QStringList* out) {
    if (v.isString()) {
        out->append(v.toString());
    } else if (v.isArray()) {
        for (const QJsonValue& x : v.toArray()) collectStrings(x, out);
    } else if (v.isObject()) {
        const QJsonObject o = v.toObject();
        for (auto it = o.constBegin(); it != o.constEnd(); ++it) collectStrings(it.value(), out);
    }
}

// Returns a copy of v with every string passed through fn. fn returns true when it changed the string.
QJsonValue mapStrings(const QJsonValue& v, const std::function<bool(QString&)>& fn, int* changed) {
    if (v.isString()) {
        QString s = v.toString();
        if (fn(s)) {
            ++*changed;
            return s;
        }
        return v;
    }
    if (v.isArray()) {
        QJsonArray a;
        for (const QJsonValue& x : v.toArray()) a.append(mapStrings(x, fn, changed));
        return a;
    }
    if (v.isObject()) {
        QJsonObject o;
        const QJsonObject src = v.toObject();
        for (auto it = src.constBegin(); it != src.constEnd(); ++it) o.insert(it.key(), mapStrings(it.value(), fn, changed));
        return o;
    }
    return v;
}

// Parses a reference file. Returns false when it is not JSON.
bool readJson(const QString& path, QJsonDocument* doc) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QJsonParseError err;
    *doc = QJsonDocument::fromJson(f.readAll(), &err);
    return err.error == QJsonParseError::NoError && !doc->isNull();
}

// True when s names rel itself, or (for a folder) something inside it.
bool names(const QString& s, const QString& rel, bool isDir) {
    return s == rel || (isDir && s.startsWith(rel + QLatin1Char('/')));
}

QString splitStem(const QString& name, QString* suffix) {
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0) {
        suffix->clear();
        return name;
    }
    *suffix = name.mid(dot);
    return name.left(dot);
}

bool copyTree(const QString& src, const QString& dst) {
    QFileInfo info(src);
    if (!info.isDir()) return QFile::copy(src, dst);
    if (!QDir().mkpath(dst)) return false;
    QDir srcDir(src);
    const QStringList entries = srcDir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QString& e : entries)
        if (!copyTree(srcDir.filePath(e), QDir(dst).filePath(e))) return false;
    return true;
}

} // namespace

QString relative(const QString& root, const QString& path) {
    return QDir(root).relativeFilePath(path);
}

bool isReferenceFile(const QString& path) {
    const QString s = QFileInfo(path).suffix().toLower();
    return s == QLatin1String("ukscene") || s == QLatin1String("ukprefab") || s == QLatin1String("ukmat") ||
           s == QLatin1String("ukshader");
}

QString uniqueName(const QString& dir, const QString& base) {
    if (!QFileInfo::exists(QDir(dir).filePath(base))) return base;
    QString suffix;
    const QString stem = splitStem(base, &suffix);
    for (int n = 2;; ++n) {
        const QString candidate = stem + QStringLiteral(" ") + QString::number(n) + suffix;
        if (!QFileInfo::exists(QDir(dir).filePath(candidate))) return candidate;
    }
}

Result checkName(const QString& name) {
    const QString t = name.trimmed();
    if (t.isEmpty()) return {false, QStringLiteral("The name is empty.")};
    if (t == QLatin1String(".") || t == QLatin1String(".."))
        return {false, QStringLiteral("That name is reserved.")};
    for (const QChar c : t) {
        if (c == QLatin1Char('/') || c == QLatin1Char('\\'))
            return {false, QStringLiteral("A name cannot contain / or \\.")};
        if (c.unicode() < 0x20) return {false, QStringLiteral("The name contains a control character.")};
    }
    return {true, QString()};
}

Result createFolder(const QString& parentDir, const QString& name, QString* created) {
    const Result ok = checkName(name);
    if (!ok.ok) return ok;
    const QString path = QDir(parentDir).filePath(name.trimmed());
    if (QFileInfo::exists(path)) return {false, QStringLiteral("A file or folder with that name already exists.")};
    if (!QDir(parentDir).mkdir(name.trimmed())) return {false, QStringLiteral("The folder could not be created.")};
    if (created) *created = path;
    return {true, QString()};
}

Result rename(const QString& root, const QString& path, const QString& newName, QString* newPath, int* rewritten) {
    const Result ok = checkName(newName);
    if (!ok.ok) return ok;
    const QString name = newName.trimmed();
    const QFileInfo before(path);
    if (!before.exists()) return {false, QStringLiteral("That file no longer exists.")};
    const QString target = QDir(before.absolutePath()).filePath(name);
    if (target == before.absoluteFilePath()) {
        if (newPath) *newPath = path;
        return {true, QString()};
    }
    if (QFileInfo::exists(target)) return {false, QStringLiteral("A file or folder with that name already exists.")};

    const bool isDir = before.isDir();
    const QString oldRel = relative(root, before.absoluteFilePath());
    if (!QDir().rename(before.absoluteFilePath(), target))
        return {false, QStringLiteral("The file could not be renamed.")};
    const QString newRel = relative(root, target);

    // Rewrite each JSON reference that names the old path (or something inside an old folder).
    int changedFiles = 0;
    for (const QString& file : referenceFiles(root)) {
        QJsonDocument doc;
        if (!readJson(file, &doc)) continue;
        int changed = 0;
        auto fn = [&](QString& s) {
            if (!names(s, oldRel, isDir)) return false;
            s = newRel + s.mid(oldRel.size());
            return true;
        };
        const QJsonValue content = doc.isArray() ? QJsonValue(doc.array()) : QJsonValue(doc.object());
        const QJsonValue mapped = mapStrings(content, fn, &changed);
        if (changed == 0) continue;
        const QJsonDocument out = mapped.isArray() ? QJsonDocument(mapped.toArray()) : QJsonDocument(mapped.toObject());
        QFile f(file);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return {false, QStringLiteral("Could not update ") + relative(root, file)};
        f.write(out.toJson(QJsonDocument::Indented));
        ++changedFiles;
    }
    if (rewritten) *rewritten = changedFiles;
    if (newPath) *newPath = target;
    return {true, QString()};
}

Result trash(const QString& path) {
    if (!QFileInfo::exists(path)) return {false, QStringLiteral("That file no longer exists.")};
    if (QFile::moveToTrash(path)) return {true, QString()};
    // A machine that has never used a desktop trash has no freedesktop trash folders yet, and Qt will not
    // create them. Make them and try once more. The file is never deleted outright.
    const QString trashDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/Trash");
    if (QDir().mkpath(trashDir + QStringLiteral("/files")) && QDir().mkpath(trashDir + QStringLiteral("/info")) &&
        QFile::moveToTrash(path))
        return {true, QString()};
    return {false, QStringLiteral("The file could not be moved to the trash.")};
}

Result importInto(const QStringList& sources, const QString& destDir, QStringList* imported) {
    QStringList failed;
    for (const QString& src : sources) {
        const QFileInfo info(src);
        if (!info.exists()) {
            failed << info.fileName();
            continue;
        }
        if (info.absolutePath() == QDir(destDir).absolutePath()) {
            failed << info.fileName() + QStringLiteral(" (already in this folder)");
            continue;
        }
        const QString target = QDir(destDir).filePath(uniqueName(destDir, info.fileName()));
        if (!copyTree(info.absoluteFilePath(), target)) {
            failed << info.fileName();
            continue;
        }
        if (imported) *imported << target;
    }
    if (failed.isEmpty()) return {true, QString()};
    return {false, QStringLiteral("Not imported: ") + failed.join(QStringLiteral(", "))};
}

QStringList usesOf(const QString& root, const QString& path) {
    QStringList out;
    if (!isReferenceFile(path) || !QFileInfo(path).isFile()) return out;
    QJsonDocument doc;
    if (!readJson(path, &doc)) return out;
    QStringList strings;
    collectStrings(doc.isArray() ? QJsonValue(doc.array()) : QJsonValue(doc.object()), &strings);
    QSet<QString> seen;
    for (const QString& s : strings) {
        if (s.isEmpty()) continue;
        const QString abs = QDir(root).absoluteFilePath(s);
        if (!QFileInfo(abs).isFile() || abs == QFileInfo(path).absoluteFilePath()) continue;
        const QString rel = relative(root, abs);
        if (!seen.contains(rel)) {
            seen.insert(rel);
            out << rel;
        }
    }
    return out;
}

QStringList usedBy(const QString& root, const QString& path) {
    QStringList out;
    const QFileInfo target(path);
    const QString rel = relative(root, target.absoluteFilePath());
    const bool isDir = target.isDir();
    const QString self = target.absoluteFilePath();
    for (const QString& file : referenceFiles(root)) {
        if (file == self || file.startsWith(self + QLatin1Char('/'))) continue;
        QJsonDocument doc;
        if (!readJson(file, &doc)) continue;
        QStringList strings;
        collectStrings(doc.isArray() ? QJsonValue(doc.array()) : QJsonValue(doc.object()), &strings);
        for (const QString& s : strings) {
            if (names(s, rel, isDir)) {
                out << relative(root, file);
                break;
            }
        }
    }
    out.removeDuplicates();
    return out;
}

} // namespace assetops
