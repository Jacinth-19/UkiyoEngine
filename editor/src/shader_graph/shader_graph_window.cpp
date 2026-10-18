#include "shader_graph_window.h"

#include "sg_codegen.h"
#include "sg_nodes.h"
#include "engine/shader_compiler.h"
#include "../material_thumbnail.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtGui/QCloseEvent>
#include <QtGui/QCursor>
#include <QtGui/QFontDatabase>
#include <QtGui/QKeySequence>
#include <QtGui/QAction>
#include <QtGui/QPixmap>
#include <QtGui/QShortcut>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QVBoxLayout>

#include <QtNodes/DataFlowGraphModel>
#include <QtNodes/DataFlowGraphicsScene>
#include <QtNodes/GraphicsView>
#include <QtNodes/NodeDelegateModel>

#include <algorithm>

namespace sg {

using QtNodes::ConnectionId;
using QtNodes::NodeId;
using QtNodes::NodeRole;
using QtNodes::NodeValidationState;

namespace {
constexpr const char* kFormat = "ukiyo-shader-graph";
const char* kFileFilter = "Ukiyo Shader Graph (*.ukshader)";
constexpr int kMaxUndo = 200;
// Edits closer together than this share one undo step, so a drag or a run of typing is one step.
constexpr qint64 kUndoGroupMs = 1000;
}

ShaderGraphWindow::ShaderGraphWindow(const QString& assetsDir, QWidget* parent)
    : QMainWindow(parent)
    , assetsDir_(assetsDir)
{
    setWindowTitle(QStringLiteral("[*]Shader Graph"));
    resize(1280, 800);

    registry_ = makeRegistry();
    model_ = std::make_unique<QtNodes::DataFlowGraphModel>(registry_);
    scene_ = new QtNodes::DataFlowGraphicsScene(*model_, this);
    scene_->setGroupingEnabled(true);   // Group Selection needs grouping on
    view_ = new QtNodes::GraphicsView(scene_);

    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    surfaceEdit_ = new QPlainTextEdit;
    fragmentEdit_ = new QPlainTextEdit;
    for (QPlainTextEdit* e : {surfaceEdit_, fragmentEdit_}) {
        e->setReadOnly(true);
        e->setFont(mono);
        e->setLineWrapMode(QPlainTextEdit::NoWrap);
    }
    errorList_ = new QListWidget;
    errorList_->setObjectName(QStringLiteral("sgErrors"));
    connect(errorList_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        if (item) selectNode(item->data(Qt::UserRole).toInt());
    });
    codeTabs_ = new QTabWidget;
    codeTabs_->setDocumentMode(true);
    codeTabs_->addTab(surfaceEdit_, QStringLiteral("Surface Function"));
    codeTabs_->addTab(fragmentEdit_, QStringLiteral("Fragment Shader"));
    codeTabs_->addTab(errorList_, QStringLiteral("Errors"));

    auto* split = new QSplitter(Qt::Horizontal);
    split->addWidget(view_);
    split->addWidget(codeTabs_);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    setCentralWidget(split);

    previewLabel_ = new QLabel(QStringLiteral("The preview renders the graph on a sphere."));
    previewLabel_->setAlignment(Qt::AlignCenter);
    previewLabel_->setWordWrap(true);
    previewLabel_->setMinimumSize(256, 256);
    previewDock_ = new QDockWidget(QStringLiteral("Preview"), this);
    previewDock_->setObjectName(QStringLiteral("sgPreview"));
    previewDock_->setWidget(previewLabel_);
    addDockWidget(Qt::RightDockWidgetArea, previewDock_);

    status_ = new QLabel;
    statusBar()->addWidget(status_, 1);

    QMenu* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    auto addAction = [this](QMenu* m, const QString& text, const QKeySequence& key, auto slot) {
        QAction* a = m->addAction(text);
        if (!key.isEmpty()) a->setShortcut(key);
        connect(a, &QAction::triggered, this, slot);
        return a;
    };
    addAction(fileMenu, QStringLiteral("New Shader Graph"), QKeySequence::New, [this] { newGraph(); });
    addAction(fileMenu, QStringLiteral("Open..."), QKeySequence::Open, [this] {
        if (!confirmDiscard()) return;
        const QString p = QFileDialog::getOpenFileName(this, QStringLiteral("Open Shader Graph"),
                                                       assetsDir_, kFileFilter);
        if (!p.isEmpty()) openPath(p);
    });
    addAction(fileMenu, QStringLiteral("Save"), QKeySequence::Save, [this] { save(); });
    addAction(fileMenu, QStringLiteral("Save As..."), QKeySequence::SaveAs, [this] { saveAs(); });
    fileMenu->addSeparator();
    addAction(fileMenu, QStringLiteral("Close"), QKeySequence::Close, [this] { close(); });

    QMenu* editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
    undoAct_ = addAction(editMenu, QStringLiteral("Undo"), QKeySequence::Undo, [this] { undo(); });
    redoAct_ = addAction(editMenu, QStringLiteral("Redo"), QKeySequence::Redo, [this] { redo(); });
    editMenu->addSeparator();
    addAction(editMenu, QStringLiteral("Duplicate Node"), QKeySequence(Qt::CTRL | Qt::Key_D),
              [this] { duplicateSelected(); });
    addAction(editMenu, QStringLiteral("Group Selection"), QKeySequence(Qt::CTRL | Qt::Key_G),
              [this] { groupSelected(); });
    addAction(editMenu, QStringLiteral("Ungroup Selection"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G),
              [this] { ungroupSelected(); });
    addAction(editMenu, QStringLiteral("Search Nodes..."), QKeySequence(), [this] { showPalette(); });
    // Space opens the node search while the canvas has focus. Text fields keep their own Space key.
    auto* space = new QShortcut(QKeySequence(Qt::Key_Space), view_);
    space->setContext(Qt::WidgetWithChildrenShortcut);
    connect(space, &QShortcut::activated, this, [this] { showPalette(); });

    QMenu* viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    addAction(viewMenu, QStringLiteral("Frame All"), QKeySequence(Qt::Key_F), [this] { view_->centerScene(); });
    viewMenu->addAction(previewDock_->toggleViewAction());

    timer_ = new QTimer(this);
    timer_->setInterval(250);
    connect(timer_, &QTimer::timeout, this, &ShaderGraphWindow::poll);
    timer_->start();

    // The preview is rendered a moment after the last edit, so a run of edits renders once.
    previewTimer_ = new QTimer(this);
    previewTimer_->setSingleShot(true);
    previewTimer_->setInterval(400);
    connect(previewTimer_, &QTimer::timeout, this, &ShaderGraphWindow::refreshPreview);
    connect(previewDock_, &QDockWidget::visibilityChanged, this, [this](bool shown) {
        if (shown) previewTimer_->start();
    });

    buildDefaultGraph();
    savedSignature_ = signature();
    poll();
    updateTitle();
}

ShaderGraphWindow::~ShaderGraphWindow()
{
    // The view and scene refer to the model, so they are destroyed before it.
    delete view_;
    view_ = nullptr;
    delete scene_;
    scene_ = nullptr;
}

void ShaderGraphWindow::buildDefaultGraph()
{
    const NodeId color = model_->addNode(QStringLiteral("Color"));
    model_->setNodeData(color, NodeRole::Position, QPointF(-300.0, -60.0));
    const NodeId master = model_->addNode(QStringLiteral("PBRMaster"));
    model_->setNodeData(master, NodeRole::Position, QPointF(120.0, -60.0));
    model_->addConnection(ConnectionId{color, 0, master, 0});
}

// The graph's nodes and connections, plus its groups. Undo, redo and the saved-state check use it.
QJsonObject ShaderGraphWindow::graphSnapshot() const
{
    QJsonObject snap;
    snap[QStringLiteral("graph")] = model_->save();
    snap[QStringLiteral("groups")] = groupsJson();
    return snap;
}

QJsonArray ShaderGraphWindow::groupsJson() const
{
    QJsonArray out;
    for (const auto& [groupId, group] : scene_->groups()) {
        if (!group) continue;
        QJsonObject g;
        g[QStringLiteral("id")] = static_cast<qint64>(groupId);
        g[QStringLiteral("name")] = group->name();
        QJsonArray nodes;
        for (NodeId n : group->nodeIDs()) nodes.append(static_cast<qint64>(n));
        g[QStringLiteral("nodes")] = nodes;
        g[QStringLiteral("locked")] = group->groupGraphicsObject().locked();
        out.append(g);
    }
    return out;
}

QByteArray ShaderGraphWindow::signature() const
{
    return QJsonDocument(graphSnapshot()).toJson(QJsonDocument::Compact);
}

void ShaderGraphWindow::poll()
{
    const QByteArray sig = signature();
    if (sig != lastSignature_) {
        lastSignature_ = sig;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        // The state before this edit goes on the undo stack, unless the edit continues a burst.
        if (!applying_ && !lastSnapshot_.isEmpty() && now - lastEditMs_ > kUndoGroupMs) {
            undoStack_.append(lastSnapshot_);
            if (undoStack_.size() > kMaxUndo) undoStack_.removeFirst();
            redoStack_.clear();
        }
        lastEditMs_ = now;
        lastSnapshot_ = graphSnapshot();
        regenerate();
    }
    updateTitle();
}

void ShaderGraphWindow::applySnapshot(const QJsonObject& snapshot)
{
    applying_ = true;
    const auto ids = model_->allNodeIds();
    for (NodeId id : ids) model_->deleteNode(id);
    model_->load(snapshot.value(QStringLiteral("graph")).toObject());
    for (const QJsonValue& g : snapshot.value(QStringLiteral("groups")).toArray()) {
        scene_->restoreGroup(g.toObject());
    }
    applying_ = false;
    // The restored state is the new baseline, so the next poll does not record it as an edit.
    lastSignature_ = signature();
    lastSnapshot_ = graphSnapshot();
    regenerate();
    updateTitle();
}

void ShaderGraphWindow::undo()
{
    if (undoStack_.isEmpty()) return;
    redoStack_.append(graphSnapshot());
    const QJsonObject previous = undoStack_.takeLast();
    applySnapshot(previous);
    lastEditMs_ = 0;   // the next edit starts a new step
}

void ShaderGraphWindow::redo()
{
    if (redoStack_.isEmpty()) return;
    undoStack_.append(graphSnapshot());
    const QJsonObject next = redoStack_.takeLast();
    applySnapshot(next);
    lastEditMs_ = 0;
}

void ShaderGraphWindow::regenerate()
{
    const CodegenResult result = generate(*model_);
    surfaceEdit_->setPlainText(result.surfaceFunction);
    fragmentEdit_->setPlainText(result.fragmentShader);
    clearNodeErrors();
    errorList_->clear();

    auto addError = [this](const QString& text, int node) {
        auto* item = new QListWidgetItem(text);
        item->setData(Qt::UserRole, node);
        errorList_->addItem(item);
    };

    if (!result.ok()) {
        status_->setStyleSheet(QStringLiteral("color:#e0a040;"));
        status_->setText(QStringLiteral("Graph: ") + result.errors.join(QStringLiteral("  ")));
        for (const QString& e : result.errors) addError(e, -1);
    } else {
        bool ok = false;
        QList<std::pair<int, QString>> glslErrors;
        const QString msg = validateWithGlslang(result.fragmentShader, ok, &glslErrors);
        for (const auto& [line, text] : glslErrors) {
            const int node = result.fragmentLineNodes.contains(line)
                                 ? static_cast<int>(result.fragmentLineNodes.value(line))
                                 : -1;
            if (node >= 0) {
                markNodeError(static_cast<unsigned int>(node), text);
                addError(QStringLiteral("%1: line %2: %3")
                             .arg(model_->nodeExists(static_cast<NodeId>(node))
                                      ? model_->delegateModel<ShaderNodeModel>(static_cast<NodeId>(node))->caption()
                                      : QStringLiteral("Node"))
                             .arg(line)
                             .arg(text),
                         node);
            } else {
                addError(QStringLiteral("Line %1: %2").arg(line).arg(text), -1);
            }
        }
        if (!ok && glslErrors.isEmpty()) addError(msg, -1);
        status_->setStyleSheet(ok ? QStringLiteral("color:#7fc97f;") : QStringLiteral("color:#e06060;"));
        status_->setText(msg);
    }

    const int n = errorList_->count();
    codeTabs_->setTabText(codeTabs_->indexOf(errorList_),
                          n ? QStringLiteral("Errors (%1)").arg(n) : QStringLiteral("Errors"));
    if (previewDock_->isVisible()) previewTimer_->start();
}

void ShaderGraphWindow::clearNodeErrors()
{
    for (unsigned int id : errorNodes_) {
        if (model_->nodeExists(id)) model_->setNodeData(id, NodeRole::ValidationState, QVariant::fromValue(NodeValidationState()));
    }
    errorNodes_.clear();
}

void ShaderGraphWindow::markNodeError(unsigned int node, const QString& message)
{
    if (!model_->nodeExists(node) || errorNodes_.contains(node)) return;   // the first error on a node is shown
    NodeValidationState state;
    state._state = NodeValidationState::State::Error;
    state._stateMessage = message;
    model_->setNodeData(node, NodeRole::ValidationState, QVariant::fromValue(state));
    errorNodes_.insert(node);
}

// Compiles the generated fragment shader with the engine's glslang (linked into ukiyo_core), so
// the editor and the engine accept exactly the same shaders. Returns a one-line status message.
// Each error's line is reported in `errors`, so a node can be marked.
QString ShaderGraphWindow::validateWithGlslang(const QString& fragmentSource, bool& ok,
                                               QList<std::pair<int, QString>>* errors)
{
    const engine::SpirvResult spv = engine::compileGlslToSpirv(fragmentSource.toStdString(), engine::ShaderStage::Fragment);
    ok = spv.ok;
    if (ok) return QStringLiteral("Shader compiles.");

    static const QRegularExpression kError(QStringLiteral("ERROR:\\s*\\d+:(\\d+):\\s*(.*)"));
    QString first;
    for (const QString& line : QString::fromStdString(spv.log).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        if (!line.contains(QStringLiteral("ERROR"))) continue;
        if (first.isEmpty()) first = line.mid(line.indexOf(QStringLiteral("ERROR")));
        const QRegularExpressionMatch m = kError.match(line);
        if (m.hasMatch() && errors) errors->append({m.captured(1).toInt(), m.captured(2).trimmed()});
    }
    return QStringLiteral("Shader error: ") + (first.isEmpty() ? QStringLiteral("compile failed") : first);
}

void ShaderGraphWindow::newGraph()
{
    if (!confirmDiscard()) return;
    const auto ids = model_->allNodeIds();
    for (NodeId id : ids) model_->deleteNode(id);
    buildDefaultGraph();
    path_.clear();
    undoStack_.clear();
    redoStack_.clear();
    lastSnapshot_ = QJsonObject();
    savedSignature_ = signature();
    poll();
    view_->centerScene();
    updateTitle();
}

bool ShaderGraphWindow::openPath(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("Open Shader Graph"), QStringLiteral("Cannot read:\n") + path);
        return false;
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    if (root.value(QStringLiteral("format")).toString() != QLatin1String(kFormat)) {
        QMessageBox::warning(this, QStringLiteral("Open Shader Graph"),
                             QStringLiteral("This file is not a Ukiyo shader graph."));
        return false;
    }
    QJsonObject snapshot;
    snapshot[QStringLiteral("graph")] = root.value(QStringLiteral("graph")).toObject();
    snapshot[QStringLiteral("groups")] = root.value(QStringLiteral("groups")).toArray();
    applySnapshot(snapshot);
    path_ = path;
    undoStack_.clear();
    redoStack_.clear();
    savedSignature_ = signature();
    view_->centerScene();
    updateTitle();
    return true;
}

bool ShaderGraphWindow::save()
{
    if (path_.isEmpty()) return saveAs();
    QJsonObject root = shaderDocument(*model_);
    root[QStringLiteral("groups")] = groupsJson();
    QFile f(path_);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, QStringLiteral("Save Shader Graph"), QStringLiteral("Cannot write:\n") + path_);
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    savedSignature_ = signature();
    updateTitle();
    return true;
}

bool ShaderGraphWindow::saveAs()
{
    QString p = QFileDialog::getSaveFileName(this, QStringLiteral("Save Shader Graph"),
                                             assetsDir_ + QStringLiteral("/New Shader Graph.ukshader"), kFileFilter);
    if (p.isEmpty()) return false;
    if (!p.endsWith(QLatin1String(".ukshader"), Qt::CaseInsensitive)) p += QStringLiteral(".ukshader");
    path_ = p;
    return save();
}

bool ShaderGraphWindow::confirmDiscard()
{
    if (signature() == savedSignature_) return true;
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("Shader Graph"));
    box.setText(QStringLiteral("Save changes to the shader graph before closing?"));
    box.setInformativeText(QStringLiteral("Your changes will be lost if you don't save them."));
    box.setStandardButtons(QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Save);
    const int r = box.exec();
    if (r == QMessageBox::Save) return save();
    return r == QMessageBox::Discard;
}

void ShaderGraphWindow::closeEvent(QCloseEvent* e)
{
    if (confirmDiscard()) e->accept();
    else e->ignore();
}

void ShaderGraphWindow::updateTitle()
{
    const QString name = path_.isEmpty() ? QStringLiteral("Untitled") : QFileInfo(path_).completeBaseName();
    setWindowTitle(QStringLiteral("[*]%1 - Shader Graph").arg(name));
    setWindowModified(signature() != savedSignature_);
    if (undoAct_) undoAct_->setEnabled(!undoStack_.isEmpty());
    if (redoAct_) redoAct_->setEnabled(!redoStack_.isEmpty());
}

// --- Editing commands -----------------------------------------------------------------------

void ShaderGraphWindow::duplicateSelected()
{
    const std::vector<NodeId> selected = scene_->selectedNodes();
    if (selected.empty()) return;
    std::vector<NodeId> copies;
    for (NodeId id : selected) {
        const QJsonObject json = model_->saveNode(id);
        if (json.isEmpty()) continue;
        // A fresh id above every existing one; loadNode keeps the id it is given.
        NodeId fresh = 0;
        for (NodeId existing : model_->allNodeIds()) fresh = std::max(fresh, existing + 1);
        QJsonObject copy = json;
        copy[QStringLiteral("id")] = static_cast<qint64>(fresh);
        model_->loadNode(copy);
        const QPointF pos = model_->nodeData(id, NodeRole::Position).toPointF();
        model_->setNodeData(fresh, NodeRole::Position, pos + QPointF(40.0, 40.0));
        copies.push_back(fresh);
    }
    scene_->clearSelection();
    for (NodeId id : copies) {
        if (auto* item = scene_->nodeGraphicsObject(id)) item->setSelected(true);
    }
}

void ShaderGraphWindow::groupSelected()
{
    if (scene_->selectedNodes().empty()) return;
    scene_->createGroupFromSelection(QStringLiteral("Group"));
}

// Ungroups every group that has a selected node, or whose frame is selected. Collected first,
// because removing the last node of a group erases that group from the scene.
void ShaderGraphWindow::ungroupSelected()
{
    const std::vector<NodeId> selected = scene_->selectedNodes();
    std::vector<NodeId> members;
    for (const auto& [groupId, group] : scene_->groups()) {
        if (!group) continue;
        const std::vector<NodeId> nodes = group->nodeIDs();
        bool touched = group->groupGraphicsObject().isSelected();
        for (NodeId n : nodes) {
            if (std::find(selected.begin(), selected.end(), n) != selected.end()) touched = true;
        }
        if (touched) members.insert(members.end(), nodes.begin(), nodes.end());
    }
    for (NodeId n : members) scene_->removeNodeFromGroup(n);
}

void ShaderGraphWindow::selectNode(int node)
{
    if (node < 0 || !model_->nodeExists(static_cast<NodeId>(node))) return;
    auto* item = scene_->nodeGraphicsObject(static_cast<NodeId>(node));
    if (!item) return;
    scene_->clearSelection();
    item->setSelected(true);
    view_->centerOn(item);
}

void ShaderGraphWindow::addNodeAt(const QString& nodeId, QPointF scenePos)
{
    const NodeId id = model_->addNode(nodeId);
    if (!model_->nodeExists(id)) return;
    model_->setNodeData(id, NodeRole::Position, scenePos);
    scene_->clearSelection();
    if (auto* item = scene_->nodeGraphicsObject(id)) item->setSelected(true);
}

// A search popup: type part of a node's name or category, then Enter or double-click to add it at
// the mouse position (or the view centre when the mouse is outside the canvas).
void ShaderGraphWindow::showPalette()
{
    if (!palette_) {
        palette_ = new QWidget(this, Qt::Popup);
        palette_->setObjectName(QStringLiteral("sgPalette"));
        auto* v = new QVBoxLayout(palette_);
        v->setContentsMargins(6, 6, 6, 6);
        paletteSearch_ = new QLineEdit;
        paletteSearch_->setPlaceholderText(QStringLiteral("Search nodes"));
        paletteList_ = new QListWidget;
        paletteList_->setMinimumSize(300, 320);
        v->addWidget(paletteSearch_);
        v->addWidget(paletteList_);
        auto accept = [this](QListWidgetItem* item) {
            if (!item) return;
            palette_->hide();
            addNodeAt(item->data(Qt::UserRole).toString(), paletteAt_);
        };
        connect(paletteSearch_, &QLineEdit::textChanged, this, [this](const QString&) { fillPalette(); });
        connect(paletteSearch_, &QLineEdit::returnPressed, this, [this, accept] {
            accept(paletteList_->currentItem() ? paletteList_->currentItem() : paletteList_->item(0));
        });
        connect(paletteList_, &QListWidget::itemActivated, this, accept);
    }
    const QPoint local = view_->viewport()->mapFromGlobal(QCursor::pos());
    const QRect canvas = view_->viewport()->rect();
    paletteAt_ = view_->mapToScene(canvas.contains(local) ? local : canvas.center());
    palette_->move(QCursor::pos());
    palette_->show();
    paletteSearch_->clear();
    paletteSearch_->setFocus();
    fillPalette();
}

// A node matches when the query is part of its caption, its id, or its category. An empty query matches all.
namespace {
bool paletteMatch(const NodeSpec& spec, const QString& query)
{
    if (query.isEmpty()) return true;
    return spec.caption.contains(query, Qt::CaseInsensitive) || spec.id.contains(query, Qt::CaseInsensitive)
           || spec.category.contains(query, Qt::CaseInsensitive);
}
}

int ShaderGraphWindow::paletteMatchCount(const QString& query) const
{
    int n = 0;
    for (const NodeSpec& spec : nodeSpecs()) n += paletteMatch(spec, query.trimmed()) ? 1 : 0;
    return n;
}

void ShaderGraphWindow::fillPalette()
{
    const QString query = paletteSearch_->text().trimmed();
    paletteList_->clear();
    for (const NodeSpec& spec : nodeSpecs()) {
        if (!paletteMatch(spec, query)) continue;
        auto* item = new QListWidgetItem(spec.caption + QStringLiteral("   (") + spec.category + QLatin1Char(')'));
        item->setData(Qt::UserRole, spec.id);
        paletteList_->addItem(item);
    }
    if (paletteList_->count()) paletteList_->setCurrentRow(0);
}

int ShaderGraphWindow::errorListCount() const
{
    return errorList_ ? errorList_->count() : 0;
}

bool ShaderGraphWindow::previewRendered() const
{
    return previewLabel_ && !previewLabel_->pixmap().isNull();
}

int ShaderGraphWindow::nodeCount() const
{
    return static_cast<int>(model_->allNodeIds().size());
}

int ShaderGraphWindow::groupCount() const
{
    int n = 0;
    for (const auto& [id, group] : scene_->groups()) n += group ? 1 : 0;
    return n;
}

// --- Preview ---------------------------------------------------------------------------------

// Writes the graph's compiled shader and a material that uses it into a temporary project, then
// renders the material on a sphere with the same path the Project panel uses for .ukmat thumbnails.
void ShaderGraphWindow::refreshPreview()
{
    if (!previewDock_->isVisible()) return;
    const CodegenResult result = generate(*model_);
    if (!result.ok()) {
        previewLabel_->setPixmap(QPixmap());
        previewLabel_->setText(QStringLiteral("The preview needs a valid graph."));
        return;
    }
    const QString dir = QDir::tempPath() + QStringLiteral("/ukiyo_shader_preview");
    QDir().mkpath(dir + QStringLiteral("/Shaders"));
    QDir().mkpath(dir + QStringLiteral("/Materials"));
    QFile shader(dir + QStringLiteral("/Shaders/Preview.ukshader"));
    if (!shader.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        previewLabel_->setText(QStringLiteral("Cannot write the preview shader."));
        return;
    }
    shader.write(QJsonDocument(shaderDocument(*model_)).toJson(QJsonDocument::Indented));
    shader.close();
    QFile material(dir + QStringLiteral("/Materials/Preview.ukmat"));
    if (material.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QJsonObject mat;
        mat[QStringLiteral("format")] = QStringLiteral("ukiyo-material");
        mat[QStringLiteral("version")] = 1;
        mat[QStringLiteral("name")] = QStringLiteral("Preview");
        mat[QStringLiteral("shader")] = QStringLiteral("Shaders/Preview.ukshader");
        mat[QStringLiteral("properties")] = QJsonObject();
        material.write(QJsonDocument(mat).toJson(QJsonDocument::Indented));
        material.close();
    }
    QString err;
    const QImage image = editor::assets::renderMaterialThumbnail(dir + QStringLiteral("/Materials/Preview.ukmat"), 256, &err);
    if (image.isNull()) {
        previewLabel_->setPixmap(QPixmap());
        previewLabel_->setText(err.isEmpty() ? QStringLiteral("The preview is unavailable.") : err);
        return;
    }
    previewLabel_->setText(QString());
    previewLabel_->setPixmap(QPixmap::fromImage(image));
}

} // namespace sg
