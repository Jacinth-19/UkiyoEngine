// State graph editor for locomotion: states as movable nodes, transitions as arrows, a side editor for the
// selected state, transition or parameter, validation, and a test pane that runs the graph's state machine.
// Edits are sent out with graphChanged(instance, anim). Undo is kept here.
#pragma once

#include <QGraphicsView>
#include <QStringList>
#include <QWidget>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "systems/components.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGraphicsScene;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QTableWidget;
class AnimGraph;

// The canvas. Forwards clicks, delete and undo keys to the graph editor.
class GraphView : public QGraphicsView {
    Q_OBJECT
public:
    explicit GraphView(QGraphicsScene* scene, QWidget* parent = nullptr);
    bool connecting = false;

signals:
    void nodeClicked(const QString& name);
    void layoutMoved();
    void deleteRequested();
    void undoRequested();
    void redoRequested();

protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    bool event(QEvent* e) override;
};

class AnimGraph : public QWidget {
    Q_OBJECT
public:
    explicit AnimGraph(QWidget* parent = nullptr);
    ~AnimGraph() override;

    // Shows an object's locomotion graph. instance < 0 shows nothing.
    void setInstance(int instance, const ukiyo::AnimComponent& anim);
    void setClipNames(const QStringList& clips);
    // Clip lengths in seconds, for exit times and fractional crossfades in the test pane.
    void setClipDurations(const std::map<std::string, float>& seconds) { durations_ = seconds; }
    const ukiyo::AnimComponent& graph() const { return graph_; }
    int instance() const { return instance_; }
    bool undo();
    bool redo();

    // Test hooks: the same operations the controls call.
    void addState(const QString& name, const QString& clip);
    void addTransition(const QString& from, const QString& to);
    void deleteState(const QString& name);
    void deleteTransition(int index);
    void renameState(const QString& from, const QString& to);
    void setDefault(const QString& name);
    void addParam(const QString& name, ukiyo::AnimParamDef::Type type, float value);
    void selectState(const QString& name);
    void selectTransition(int index);
    void createFromLegacy();
    QStringList problems() const;
    // Test pane: the state the test machine is in, and drives.
    QString testState() const;
    void testSetFloat(const QString& name, float v);
    void testAdvance(float seconds);
    // Blend trees: these act on the named state, without the side panel. Motion count is -1 for an unknown state.
    void setStateBlend(const QString& state, ukiyo::BlendKind kind, const QString& paramX, const QString& paramY);
    void addMotion(const QString& state, const QString& clip, float x, float y);
    int motionCount(const QString& state) const;

signals:
    void graphChanged(int instance, const ukiyo::AnimComponent& anim);

private:
    void commit(const ukiyo::AnimComponent& next, const QString& what, bool rebuildTables = true);
    void applyGraph(const ukiyo::AnimComponent& next, const QString& what, bool rebuildTables);
    void refreshAll(bool rebuildTables);
    void rebuildScene();
    void refreshSide(bool rebuildTables);
    void refreshParams();
    void refreshProblems();
    void rebuildTestControls();
    void rebuildTestMachine();
    void onSceneSelection();
    void onNodePicked(const QString& name);
    void onLayoutMoved();
    void onDeleteKey();
    void writeConditions();
    void writeBlendKind();
    void writeBlendParams();
    void writeMotions();
    void refreshBlendControls(int stateIndex, bool rebuildTables);
    void writeParams();
    void onRenameEdited();
    QStringList paramNames() const;
    QStringList problemList() const;
    QString uniqueStateName() const;

    ukiyo::AnimComponent graph_;
    int instance_ = -1;
    QStringList clips_;
    std::vector<ukiyo::AnimComponent> undo_;
    std::vector<ukiyo::AnimComponent> redo_;
    QString selState_;
    int selTrans_ = -1;
    bool refreshing_ = false;
    QString connectFrom_;

    QCheckBox* customBox_ = nullptr;
    QPushButton* connectBtn_ = nullptr;
    QPushButton* addStateBtn_ = nullptr;
    QPushButton* deleteBtn_ = nullptr;
    QPushButton* defaultBtn_ = nullptr;
    QPushButton* legacyBtn_ = nullptr;
    QLabel* hint_ = nullptr;
    QGraphicsScene* scene_ = nullptr;
    GraphView* view_ = nullptr;

    QStackedWidget* side_ = nullptr;
    QLineEdit* stateName_ = nullptr;
    QComboBox* stateClip_ = nullptr;
    QComboBox* blendKind_ = nullptr;     // Single clip, or one of the blend-tree kinds
    QComboBox* paramX_ = nullptr;
    QComboBox* paramY_ = nullptr;
    QTableWidget* motions_ = nullptr;    // one row per motion: clip, x, y
    QPushButton* addMotionBtn_ = nullptr;
    QPushButton* delMotionBtn_ = nullptr;
    QPushButton* autoMotionBtn_ = nullptr;   // Automate Thresholds: spreads the motions' X evenly
    QLabel* blendHint_ = nullptr;
    QString testLastBlend_;              // last blend text logged in the test pane, to skip repeats
    QCheckBox* stateDefault_ = nullptr;
    QLabel* transRoute_ = nullptr;
    QDoubleSpinBox* fade_ = nullptr;
    QCheckBox* exitCheck_ = nullptr;
    QDoubleSpinBox* exitTime_ = nullptr;
    QCheckBox* fixed_ = nullptr;
    QDoubleSpinBox* offset_ = nullptr;
    QCheckBox* selfCheck_ = nullptr;
    std::map<std::string, float> durations_;
    QDoubleSpinBox* dwell_ = nullptr;
    QTableWidget* conds_ = nullptr;
    QPushButton* addCondBtn_ = nullptr;
    QPushButton* delCondBtn_ = nullptr;
    QPushButton* deleteTransBtn_ = nullptr;
    QTableWidget* params_ = nullptr;
    QPushButton* addParamBtn_ = nullptr;
    QPushButton* delParamBtn_ = nullptr;
    QListWidget* problems_ = nullptr;
    QLabel* status_ = nullptr;

    std::unique_ptr<ukiyo::AnimStateMachine> test_;
    QWidget* testBox_ = nullptr;
    QWidget* testControls_ = nullptr;
    QLabel* testStateLabel_ = nullptr;
    QListWidget* testLog_ = nullptr;
    QStringList testParamNames_;
};
