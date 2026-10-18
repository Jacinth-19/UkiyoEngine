// Widget helpers shared by the editor's windows: the material icon, and a collapsible Inspector section.
#pragma once

#include "foldout.h"
#include "icons.h"

#include <QFormLayout>
#include <QIcon>
#include <QString>
#include <QVBoxLayout>

namespace editor {

// Icon from the editor's Material Symbols subset, in the standard UI grey.
inline QIcon uiIcon(const char* name, int px = 16) {
    return materialIcon(name, iconColor(), px);
}

// A collapsible section of the Inspector, in the Unity style: a header row with an arrow, and a
// body that hides when the header is clicked.
// Adds a foldout section to `v` and returns its form layout.
inline QFormLayout* addSection(QVBoxLayout* v, const QString& title) {
    QWidget* body = new QWidget;
    QFormLayout* form = new QFormLayout(body);
    form->setContentsMargins(14, 6, 10, 8);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(5);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    v->addWidget(new Foldout(title, body));
    return form;
}

} // namespace editor
