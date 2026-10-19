// "New project" for the Ukiyo Hub: a name, a folder, and one of the built-in templates. The dialog
// only validates the choices; the hub creates the project behind the splash screen.
#pragma once

#include <QDialog>
#include <QString>
#include <QVector>

class QLabel;
class QLineEdit;
class QPushButton;

namespace editor::hub {

class NewProjectDialog : public QDialog {
public:
    explicit NewProjectDialog(const QString& preselectedTemplate = QString(), QWidget* parent = nullptr);

    QString projectName() const;
    QString projectRoot() const;
    QString templateId() const;

protected:
    void accept() override;

private:
    void browse();
    void chooseTemplate(const QString& id);
    void updatePreview();
    void showError(const QString& text);

    QLineEdit* name_ = nullptr;
    QLineEdit* location_ = nullptr;
    QLabel* preview_ = nullptr;
    QLabel* error_ = nullptr;
    QString templateId_;
    QVector<QPushButton*> templateButtons_;
};

} // namespace editor::hub
