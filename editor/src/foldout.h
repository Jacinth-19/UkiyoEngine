// A collapsible section header with a disclosure arrow, shared by Project Settings, Preferences,
// and the Inspector. The body is any widget; the header toggles its visibility.
#pragma once

#include "icons.h"

#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

namespace editor {

class Foldout : public QWidget {
public:
    Foldout(const QString& title, QWidget* body, QWidget* parent = nullptr) : QWidget(parent), body_(body) {
        QVBoxLayout* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(0);
        head_ = new QToolButton;
        head_->setObjectName(QStringLiteral("foldoutHeader"));
        head_->setText(title);
        head_->setCheckable(true);
        head_->setChecked(true);
        head_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        head_->setFocusPolicy(Qt::NoFocus);
        head_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        v->addWidget(head_);
        v->addWidget(body_);
        connect(head_, &QToolButton::toggled, this, [this](bool open) { setOpen(open); });
        setOpen(true);
    }

private:
    void setOpen(bool open) {
        body_->setVisible(open);
        head_->setIcon(materialIcon(open ? "keyboard_arrow_down" : "keyboard_arrow_right", iconColor()));
    }

    QToolButton* head_ = nullptr;
    QWidget* body_ = nullptr;
};

}  // namespace editor
