#pragma once

// Shader Graph editor window: a QtNodes canvas on the left and the generated GLSL on the right.
// Graphs are saved as .ukshader files (JSON) in the project's assets folder.

#include <QtCore/QByteArray>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QPointF>
#include <QtCore/QSet>
#include <QtCore/QString>
#include <QtWidgets/QMainWindow>

#include <memory>
#include <utility>

class QAction;
class QDockWidget;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QTabWidget;
class QTimer;
class QWidget;

namespace QtNodes {
class DataFlowGraphModel;
class DataFlowGraphicsScene;
class GraphicsView;
class NodeDelegateModelRegistry;
}

namespace sg {

class ShaderGraphWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit ShaderGraphWindow(const QString& assetsDir, QWidget* parent = nullptr);
    ~ShaderGraphWindow() override;

    bool openPath(const QString& path);

    // Editing commands. The Edit menu and the canvas use them; the selftest drives them too.
    void undo();
    void redo();
    bool canUndo() const { return !undoStack_.isEmpty(); }
    bool canRedo() const { return !redoStack_.isEmpty(); }
    void duplicateSelected();
    void groupSelected();
    void ungroupSelected();
    void selectNode(int node);
    void addNodeAt(const QString& nodeId, QPointF scenePos);
    void showPalette();
    int paletteMatchCount(const QString& query) const;   // nodes the search popup lists for `query`

    // Per-node compile errors, shown on the node itself. A node's error clears on the next graph edit.
    void markNodeError(unsigned int node, const QString& message);

    // Read-only state, for the selftest.
    int nodeCount() const;
    int groupCount() const;
    int errorListCount() const;
    int errorNodeCount() const { return static_cast<int>(errorNodes_.size()); }
    bool previewRendered() const;
    QtNodes::DataFlowGraphModel* graph() const { return model_.get(); }
    QtNodes::DataFlowGraphicsScene* canvas() const { return scene_; }

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void newGraph();
    bool save();
    bool saveAs();
    bool confirmDiscard();
    void buildDefaultGraph();
    QByteArray signature() const;
    void poll();                       // detects graph edits and refreshes the GLSL
    void regenerate();
    QString validateWithGlslang(const QString& fragmentSource, bool& ok,
                                QList<std::pair<int, QString>>* errors);
    void updateTitle();

    // Graph state: nodes, connections and groups together. Undo and redo store these snapshots.
    QJsonObject graphSnapshot() const;
    QJsonArray groupsJson() const;
    void applySnapshot(const QJsonObject& snapshot);
    void clearNodeErrors();
    void fillPalette();                // lists the nodes that match the search text
    void refreshPreview();             // renders the Preview dock from the current graph

    QString assetsDir_;
    QString path_;                     // empty for an unsaved graph
    std::shared_ptr<QtNodes::NodeDelegateModelRegistry> registry_;
    std::unique_ptr<QtNodes::DataFlowGraphModel> model_;
    QtNodes::DataFlowGraphicsScene* scene_ = nullptr;
    QtNodes::GraphicsView* view_ = nullptr;

    QTabWidget* codeTabs_ = nullptr;
    QPlainTextEdit* fragmentEdit_ = nullptr;
    QPlainTextEdit* surfaceEdit_ = nullptr;
    QListWidget* errorList_ = nullptr;
    QLabel* status_ = nullptr;
    QTimer* timer_ = nullptr;

    QAction* undoAct_ = nullptr;
    QAction* redoAct_ = nullptr;
    QDockWidget* previewDock_ = nullptr;
    QLabel* previewLabel_ = nullptr;
    QTimer* previewTimer_ = nullptr;
    QWidget* palette_ = nullptr;
    QLineEdit* paletteSearch_ = nullptr;
    QListWidget* paletteList_ = nullptr;
    QPointF paletteAt_;

    QByteArray lastSignature_;         // graph state the GLSL was generated from
    QByteArray savedSignature_;        // graph state on disk (or at creation)
    QJsonObject lastSnapshot_;         // graph state at the last poll
    QList<QJsonObject> undoStack_;     // states before each undo step, oldest first
    QList<QJsonObject> redoStack_;
    qint64 lastEditMs_ = 0;            // time of the last edit; edits within a second share a step
    bool applying_ = false;            // true while an undo, redo or load replaces the graph
    QSet<unsigned int> errorNodes_;    // nodes currently marked with a compile error
};

} // namespace sg
