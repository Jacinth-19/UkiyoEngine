#include "inspector_filter.h"

#include "foldout.h"

#include "engine/property_search.h"

#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>

namespace editor {
namespace {

// The text a row's field shows, as far as the filter is concerned: the widget's own text when it has one, and otherwise
// whatever its children say (a vector row is three spin boxes, a colour row is a swatch and a hex line).
QString valueText(QWidget* w, int depth = 0) {
    if (!w || depth > 3) return QString();
    if (auto* e = qobject_cast<QLineEdit*>(w)) return e->text();
    if (auto* s = qobject_cast<QAbstractSpinBox*>(w)) return s->text();
    if (auto* c = qobject_cast<QComboBox*>(w)) return c->currentText();
    if (auto* cb = qobject_cast<QCheckBox*>(w)) return cb->text();
    if (auto* l = qobject_cast<QLabel*>(w)) return l->text();
    QString out;
    for (QWidget* child : w->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
        const QString t = valueText(child, depth + 1);
        if (!t.isEmpty()) out += out.isEmpty() ? t : QStringLiteral(" ") + t;
    }
    return out;
}

// The label a row shows: the label widget's text, or a spanning row's own text.
QString labelText(QWidget* w) {
    if (auto* l = qobject_cast<QLabel*>(w)) return l->text();
    if (auto* cb = qobject_cast<QCheckBox*>(w)) return cb->text();
    if (auto* b = qobject_cast<QAbstractButton*>(w)) return b->text();
    if (auto* e = qobject_cast<QLineEdit*>(w)) return e->text();
    return QString();
}

} // namespace

void InspectorFilter::reset() {
    open_.clear();
    page_ = nullptr;
    query_.clear();
    shown_ = 0;
}

size_t InspectorFilter::apply(QWidget* page) {
    if (!page) return shown_;
    if (page != page_) {
        page_ = page;
        open_.clear();
    }
    const std::vector<Foldout*> folds = page->findChildren<Foldout*>();
    if (folds.empty()) return shown_;   // a message page ("Select an object..."): nothing to narrow

    // One row per form row, in page order. The two roles can be the same widget: addRow(QWidget*) puts the row in the
    // spanning role, and then hiding it once is hiding the row.
    struct Row {
        QWidget* label = nullptr;
        QWidget* field = nullptr;
    };
    std::vector<Row> rows;
    std::vector<ukiyo::propsearch::Field> fields;
    std::vector<int> rowSection;   // which foldout each row belongs to
    for (int fi = 0; fi < static_cast<int>(folds.size()); ++fi) {
        const QString title = folds[fi]->title();
        for (QFormLayout* form : folds[fi]->body()->findChildren<QFormLayout*>()) {
            for (int r = 0; r < form->rowCount(); ++r) {
                Row row;
                if (QLayoutItem* li = form->itemAt(r, QFormLayout::LabelRole)) row.label = li->widget();
                if (QLayoutItem* fi2 = form->itemAt(r, QFormLayout::FieldRole)) row.field = fi2->widget();
                if (QLayoutItem* si = form->itemAt(r, QFormLayout::SpanningRole)) {
                    row.label = si->widget();
                    row.field = nullptr;
                }
                ukiyo::propsearch::Field f;
                f.section = title.toStdString();
                f.label = labelText(row.label).toStdString();
                f.value = valueText(row.field ? row.field : row.label).toStdString();
                rows.push_back(row);
                fields.push_back(std::move(f));
                rowSection.push_back(fi);
            }
        }
    }
    if (rows.empty()) return shown_;

    query_ = box_ ? box_->text() : QString();
    const ukiyo::propsearch::Filter result = ukiyo::propsearch::filter(fields, query_.toStdString());
    shown_ = result.shown;

    // Which sections still have a row.
    std::vector<bool> alive(folds.size(), false);
    for (size_t i : result.sections) alive[static_cast<size_t>(rowSection[i])] = true;

    if (!result.active) {
        // Everything back the way it was, including the sections the user had collapsed before searching.
        for (const auto& [fold, wasOpen] : open_)
            if (fold) static_cast<Foldout*>(fold.data())->setOpen(wasOpen);
        open_.clear();
    } else if (open_.empty()) {
        // First key of a search: remember what was open, so clearing the box is not a layout change.
        for (Foldout* f : folds) open_.emplace_back(f, f->isOpen());
    }

    for (size_t i = 0; i < rows.size(); ++i) {
        const bool visible = result.visible[i];
        if (rows[i].label) rows[i].label->setVisible(visible);
        if (rows[i].field && rows[i].field != rows[i].label) rows[i].field->setVisible(visible);
    }
    for (int fi = 0; fi < static_cast<int>(folds.size()); ++fi) {
        folds[fi]->setVisible(alive[static_cast<size_t>(fi)]);
        if (result.active && alive[static_cast<size_t>(fi)]) folds[fi]->setOpen(true);
    }
    return shown_;
}

} // namespace editor
