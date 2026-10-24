// The widget half of the Inspector's search (Flax 1.13's Properties panel search; the rule itself is
// src/engine/property_search.h). It walks a page of `Foldout` sections and `QFormLayout` rows, asks the rule which rows
// match, and hides the rest — opening a section whose rows matched, and remembering what was open so clearing the box
// puts the layout back the way the user left it.
//
// It lives here rather than in MainWindow so that the editor's screenshot mode can draw the panel without an engine
// session, and so there is exactly one implementation of "what counts as a row".
#pragma once

#include <QPointer>
#include <QString>
#include <QWidget>

#include <utility>
#include <vector>

class QLineEdit;

namespace editor {

class InspectorFilter {
public:
    // `box` is the filter field. It is not owned and not connected: the caller feeds text changes to apply().
    explicit InspectorFilter(QLineEdit* box = nullptr) : box_(box) {}
    void setBox(QLineEdit* box) { box_ = box; }

    // Applies the box's current text to `page`. Returns how many labelled rows are still shown; the caller reports a
    // zero to the user, because a blank panel with no explanation reads as a bug.
    size_t apply(QWidget* page);

    // Forgets the remembered open states (a new page has its own).
    void reset();

    // The text the last apply() used, for a caller that wants to log or test it.
    const QString& query() const { return query_; }
    // The number of labelled rows the last apply() left visible.
    size_t shown() const { return shown_; }

private:
    QLineEdit* box_ = nullptr;
    QWidget* page_ = nullptr;
    std::vector<std::pair<QPointer<QWidget>, bool>> open_;   // foldout -> whether it was open before searching
    QString query_;
    size_t shown_ = 0;
};

} // namespace editor
