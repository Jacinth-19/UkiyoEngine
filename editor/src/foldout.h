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
        title_ = title;
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
        connect(head_, &QToolButton::toggled, this, [this](bool open) { applyOpen(open); });
        applyOpen(true);
    }

    // What the Inspector's search needs: the header's text and the body it shows, and a way to open a section whose
    // rows matched, so a hit three sections down is visible rather than merely present.
    const QString& title() const { return title_; }
    QWidget* body() const { return body_; }
    bool isOpen() const { return head_->isChecked(); }
    void setOpen(bool open) {
        if (head_->isChecked() != open) head_->setChecked(open);   // the toggled() handler does the rest
        else applyOpen(open);
    }

private:
    void applyOpen(bool open) {
        body_->setVisible(open);
        head_->setIcon(materialIcon(open ? "keyboard_arrow_down" : "keyboard_arrow_right", iconColor()));
    }

    QToolButton* head_ = nullptr;
    QWidget* body_ = nullptr;
    QString title_;
};

}  // namespace editor
