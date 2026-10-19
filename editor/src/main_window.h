// Ukiyo World Editor main window, laid out like the Unity Editor: Hierarchy on the left, the Scene
// and Game views in the centre, Inspector and Project Settings on the right, and Project, Animation
// and Console along the bottom. Rendering happens on the RenderWorker thread; this window only
// sends commands and shows the frames that come back.
#pragma once

#include <glm/glm.hpp>
#include <vector>

#include <array>
#include <memory>
#include <functional>
#include <utility>
#include <vector>

#include <QAction>
#include <optional>

#include <QCheckBox>
#include <QDockWidget>
#include <QPointer>
#include <functional>
#include "scene_gizmo.h"
#include "engine_session.h"
#include "engine/terrain.h"
#include <map>
#include <set>
#include "asset_ops.h"
#include <QAbstractItemView>
#include <QElapsedTimer>
#include <QJsonObject>
#include "engine/project_settings.h"
#include <QMenu>
#include <QImage>
#include <QLabel>
#include <QMainWindow>
#include <QMetaObject>
#include <QModelIndex>
#include <QPoint>
#include <QSize>
#include <QThread>
#include <QTimer>
#include <QWidget>

#include "engine/camera.h"
#include "engine/water.h"
#include "engine/renderer.h"
#include "prefab_file.h"
#include "anim_graph.h"
#include "anim_panel.h"
#include "render_worker.h"

class QButtonGroup;
class QCloseEvent;
class QComboBox;
class QMenu;
class QStackedWidget;
class QDoubleSpinBox;
class QFileSystemModel;
class QLineEdit;
class QListView;
class QListWidget;
class QPushButton;
class QScrollArea;
class QSlider;
class QTabWidget;
class QToolButton;
class QTreeView;
class QTreeWidget;
class QTreeWidgetItem;

#include "prefs_dialog.h"

class QVBoxLayout;

// The undo snapshot lists the terrain records of its step under this key (item 18, step 5).
inline const QString kTerrainUndoKey = QStringLiteral("terrainUndo");

namespace editor {

class MaterialInspector;

// One rendered view. The Scene view accepts mouse input to move the camera.
class RenderView : public QLabel {
    Q_OBJECT
public:
    explicit RenderView(bool interactive, QWidget* parent = nullptr);

    // `fast` selects nearest-neighbour scaling, used during playback to keep the GUI responsive.
    void setFrame(const QImage& frame, bool fast);
    // Where the displayed frame sits: centred in the view, at its own size.
    QRect frameRect() const;

    // Scene view overlay hooks. onPress returns true when it takes a left press (a gizmo handle); the
    // drag then goes to onDrag until the button is released, and onRelease ends it.
    std::function<bool(QPointF)> onPress;
    std::function<void(QPointF)> onDrag;
    std::function<void(QPointF)> onRelease;
    std::function<void(QPointF)> onHover;
    std::function<void(QPainter&)> onPaint;

signals:
    void orbitDragged(float dx, float dy);
    void panDragged(float dx, float dy, float viewportHeight);
    void zoomed(float steps);
    void sized();

protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void paintEvent(QPaintEvent* e) override;

private:
    void showScaled();

    bool interactive_;
    bool fast_ = false;
    QImage frame_;
    QPoint lastPos_;
    Qt::MouseButtons drag_;
    bool overlayDrag_ = false;   // a press taken by onPress is being dragged
};


// The tonemap comparison dialog shows every operator in the Tonemap menu, in menu order.
inline constexpr int kTonemapCount = 10;
inline const char* const kTonemapNames[kTonemapCount] = {
    "ACES fit", "PBR Neutral", "AgX", "Custom Anime", "Linear clamp (debug)",
    "Reinhard extended", "Hejl-Burgess-Dawson", "Hable (Uncharted 2)", "Lottes", "Uchimura (GT)"};

// The prefab open in prefab mode. Its objects are plain instances while the stage is open.
struct PrefabStage {
    QString file;            // absolute path of the .ukprefab
    QString snapshot;        // temporary .ukscene holding the scene as it was before the stage
    QString savedScenePath;  // scenePath_ before the stage, restored on exit
    bool savedDirty = false; // sceneDirty_ before the stage, restored on exit
    bool dirty = false;      // edits since the last save
    QStringList keys;        // the prefab node each instance shows, by instance index
    QString rootKey;         // key of the top prefab's root
    QStringList removedKeys; // nodes deleted in this stage; removed from the file on save
    std::vector<prefab::FlatNode> nodes;   // the flattened file as of the last load or save
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    // `projectRoot` is the project to open. Empty uses the engine checkout (UKIYO_ROOT), which is
    // also how the editor behaved before projects existed.
    explicit MainWindow(const QString& projectRoot = QString(), QWidget* parent = nullptr);
    ~MainWindow() override;

    // Progress for the splash: the current stage and a 0..1 fraction. Runs on the GUI thread.
    using LoadProgress = std::function<void(const QString& stage, float fraction)>;

    // Starts the engine, loads the animation library and the project's scene (or the default scene).
    bool bootstrap(QString* error, const LoadProgress& progress = LoadProgress());

    // Runs the built-in checks (`ukiyo_editor --selftest`) and prints PASS/FAIL lines.
    // Returns the number of failed checks.
    int runSelfTest();

private slots:
    void openModelDialog();
    void addModelDialog();
    void openAnimDialog();
    void togglePlay(bool playing);
    void togglePlayback();
    void tick();
    void onViewTabChanged(int index);
    void onHierarchyChanged(QTreeWidgetItem* current);
    void onClipChanged(int index);
    void onScrub(float fraction);
    void onBrowserActivated(const QModelIndex& index);
    void onFrameReady(QImage image, bool game, float clipLength, double renderMs, uint32_t drawCalls, uint32_t triangles);
    void onSceneChanged();
    void startBenchmark();              // Tools > Benchmark Scene
    void finishBenchmark();
    // Game view overlay: the stats box and the selected object's handles, drawn over the frame.
    void paintGameOverlay(QPainter& p);
    // Maximize on Play: hides the panels while playing and restores them on stop.
    void enterMaximizedGame();
    void leaveMaximizedGame();
    void saveScreenshot();
    void saveWindowCapture();
    void resetViewportCamera();
    void focusSelected();
    void resetTime();
    void resetProjectSettings();
    // Project settings pages (Input, Tags and Layers, Time, Physics, Build). Each page writes
    // ProjectSettings/ProjectSettings.json when it changes, and sends the values to the play worker.
    void fillInputPage(QVBoxLayout* v);
    void fillTagsPage(QVBoxLayout* v);
    void fillTimePage(QVBoxLayout* v);
    void fillPhysicsPage(QVBoxLayout* v);
    void fillBuildPage(QVBoxLayout* v);
    void loadProjectSettingsFile();
    void saveProjectSettingsFile();
    // Forwards key presses from the Game view to the running world, so scripts can read actions.
    bool eventFilter(QObject* watched, QEvent* e) override;
    void about();
    void newScene();
    void openScene();
    bool saveScene();
    bool saveSceneAs();
    void openPreferences();
    void openProjectSettings();
    void setBrowserView(int view);

private:
    enum class LogLevel { Info, Warning, Error };
    struct LogEntry {
        LogLevel level;
        QString text;
        QString source;   // project file the line names, resolved when it was logged; empty if none
    };

    // Runs `fn` on the render thread and waits for it. Use for loads and queries.
    template <typename F>
    void runOnWorker(F&& fn) {
        QMetaObject::invokeMethod(worker_, std::forward<F>(fn), Qt::BlockingQueuedConnection);
    }
    // Queues `fn` on the render thread. Calls run in the order they were posted.
    void postToWorker(std::function<void()> fn);

    void buildCentral();
    void buildToolbar();
    void buildDocks();
    void buildMenus();
    void buildStatusBar();
    QWidget* buildProjectSettings();
    QWidget* buildProjectBrowser();
    QWidget* buildAnimation();
    QWidget* buildConsole();

    bool loadAnimLibrary(const QString& path);
    void populateClips(const std::vector<ClipInfo>& clips);
    // Scene root Inspector: Play controls, instance picker, and one section per game system.
    void addSystemsSections(QVBoxLayout* v, const std::vector<int>& multi = {});
    // One Inspector page for several selected model instances.
    void showMultiInstances(const std::vector<int>& indices);
    void loadModel(const QString& path);
    // Scene instances. The GUI describes only the active one (nodes, head, name).
    void adoptActiveModel(ModelInfo info, const QString& path);
    void addModelToScene(const QString& path);
    void activateInstance(int index);
    void rebuildModelsMenu();
    QMenu* modelsMenu_ = nullptr;

    // Prefabs. A prefab instance is an instance linked to a .ukprefab; keys it overrides win over the prefab.
    void instancePrefabDialog();
    void addPrefabToScene(const QString& prefabPath);
    void editActiveTransform();
    // Sets the active instance's placement and records overrides against its prefab.
    void setInstanceTransform(int index, const float position[3], float yawDeg);
    void applyOverridesToPrefab();
    void revertOverrides();
    // Apply or revert the given override keys ("translation", "yawDeg") of one instance.
    void applyOverridesFor(int index, const QStringList& keys);
    void revertOverridesFor(int index, const QStringList& keys);
    // Inspector page for one instance: transform with per-property revert, and the Overrides list.
    void showInstanceProperties(int index);
    void selectInstanceRow(int index);
    void scheduleInstanceRefresh(int index);
    // Sets bold on instance rows that have overrides, without rebuilding the tree.
    void updateInstanceRowMarkers();
    int inspectedInstance_ = -1;
    // Recomputes every linked instance's placement from its prefab file. Keys an instance overrides keep its value.
    void syncLinkedInstances();
    // Creates a .ukprefab from the scene's unlinked objects, and links those objects to it.
    void createPrefabFromScene();
    // Writes the prefab to path and links the objects. Returns false (with a message) when nothing qualifies.
    bool writePrefabFromScene(const QString& path);
    // Prefab mode: the prefab is edited as its own set of objects. The scene is held in a snapshot until exit.
    void editPrefabDialog();
    bool enterPrefabStage(const QString& file);
    bool savePrefabStage();
    void exitPrefabStage(bool save);
    // Asks to save an open prefab. Returns false if the user cancels. Also exits prefab mode when it returns true.
    bool leavePrefabStage();
    // Adds a reference to refPath as a child of file's root, at the origin. Refuses cycles.
    bool addPrefabReference(const QString& file, const QString& refPath);
    // Prefab mode objects. Add and delete rebuild the stage's scene from the instance list. Save writes them
    // into the file. Keys starting with "new:" are objects added in this stage and not saved yet.
    void addModelToPrefabStage(const QString& modelPath);
    // A variant is a prefab whose root references a base prefab. Its edits to the base are stored in the reference.
    bool writeVariantFile(const QString& base, const QString& path, QString* err);
    void createVariantDialog();
    void deletePrefabStageInstance(int index);
    bool reloadPrefabStage(std::vector<InstanceInfo> list, const QStringList& keys, int select);
    // The status bar's breadcrumb: the prefab chain of the selected object, for example "Outer > Inner > c1".
    void updatePrefabCrumb();
    QLabel* prefabCrumb_ = nullptr;
    void addPrefabReferenceDialog();
    std::unique_ptr<PrefabStage> prefabStage_;
    void rebuildHierarchy();
    void showPropertiesFor(QTreeWidgetItem* item);
    // The Inspector page. While the Inspector is locked, the current page stays read-only and new pages are dropped.
    void setInspectorPage(QWidget* page);
    void setInspectorLocked(bool on);
    bool inspectorLocked_ = false;
    QToolButton* lockButton_ = nullptr;
    void showNodeProperties(int node);
    void showCameraProperties(ukiyo::OrbitCamera* cam, const QString& title, const char* icon);
    void showLightProperties();
    void applyLightSpins();
    void showSceneProperties();
    // Water volume section of the scene page (item 16). Edits push the volume to the render worker.
    void addWaterSection(QVBoxLayout* v);
    void pushWater();
    void syncCameraSpins();
    void applyCameraSpins();
    void applyNodeTransform();
    void setNodeMaterial(int node, const QString& projectRelativeUkmat);
    // Turns the MMD-style toon look on or off for every mesh node at or below `node`.
    void setNodeMmdToon(int node, bool on);
    void focusNode(int node);
    void frameCamera(ukiyo::OrbitCamera& cam);
    void onEditorCameraMoved();

    void refresh(float dt = 0.0f);
    void pumpRender();
    void updateRenderSize();
    QSize renderSizeFor(QSize widgetSize) const;
    bool gameViewActive() const;
    void syncTimeline();
    float clipLength() const { return clipLength_; }
    void updateStatus();
    void updateWindowTitle();
    QString sceneTitle() const;
    void markDirty();
    bool confirmDiscardScene();
    bool writeScene(const QString& path);
    // Colour grading LUT (project-relative PNG, "" = none) and the tonemapper comparison.
    void setGradingLut(const QString& projectRelative);
    void openTonemapCompare();
    void renderTonemapComparison(const std::array<QLabel*, kTonemapCount>& cells, QLabel* status);
    QString gradingLut_;   // project-relative path of the loaded LUT, empty when none
    bool readScene(const QString& path);
    // The scene as its file document, and loading one back. readScene, the undo history and the
    // model clipboard all go through these.
    QJsonObject sceneDocument();
    bool applySceneDocument(const QJsonObject& root, const QString& source);

    // Undo history. beginEdit() records the scene before an edit; returns false when nothing was
    // recorded. Edits with the same non-empty key within 0.8 s share one step (a dragged spin box).
    bool beginEdit(const QString& key = QString());
    void undoEdit();
    void redoEdit();
    void updateEditActions();
    // Edit menu on model instances (rows listed under the scene in the Hierarchy).
    int selectedInstance() const;        // instance index of the selected row, or -1
    void copySelected();
    void cutSelected();
    void pasteClipboard();
    void duplicateSelected();
    void deleteSelected();
    void createEmptyObject();
    void createTerrainObject();
    void renameInstance(int index, const QString& name);
    QMenu* hierarchyContextMenu(QTreeWidgetItem* item);
    void buildGizmoToolbar();
    void setGizmoTool(gizmo::Tool tool);
    bool gizmoTarget(int* index, InstanceInfo* info);
    // The selected model instance, whichever view is showing.
    bool selectionTarget(int* index, InstanceInfo* info);
    bool gizmoPress(QPointF s);
    // Terrain editing (item 18, steps 4 to 6). A stroke is one undo step: its records are listed under "terrainUndo".
    bool terrainPress(QPointF s);
    void selfTestTerrainEdits(const std::function<void(bool, const QString&, const QString&)>& check);
    bool terrainStrokeDab(QPointF s);
    void terrainRelease();
    bool applyTerrainRecord(int id, bool useAfter);
    void pruneTerrainRecords();
    void runErosion(int index);
    void applyTerrainSettings(int index, const ukiyo::TerrainSettings& ts, const QString& key);
    void addTerrainInspector(QVBoxLayout* layout, int index, const ukiyo::TerrainSettings& ts);
    void gizmoDrag(QPointF s);
    void gizmoRelease();
    void gizmoHover(QPointF s);
    void cancelGizmoDrag();
    void paintGizmo(QPainter& p);
    void paintColliders(QPainter& p);     // Scene view: physics colliders as wireframes (Physics > Show colliders)
    void paintNavPreview(QPainter& p);    // Scene view: the last agent path preview
    // Find and Replace on object names. findInNames returns the next matching index after `from`, wrapping, or -1.
    // replaceInNames renames every match in one undo step and returns how many changed.
    int findInNames(const QString& text, bool matchCase, int from);
    int replaceInNames(const QString& find, const QString& replacement, bool matchCase);
    void openFindReplace();
    void updateMemoryStatus();
    void applyInstanceTransform(int index, const float position[3], float yawDeg);
    void copyInstance(int index);
    void duplicateInstance(int index);
    void deleteInstance(int index);
    void addInstanceEntry(const QJsonObject& entry, bool keepNodes);
    void applyLayoutPreset(const QString& name);
    void buildLayoutMenu(QMenu* menu);
    void saveCustomLayout();
    void deleteCustomLayout(const QString& name);
    void restoreStartupLayout();
    void applyPreferences();
    QWidget* buildGamePage();
    QSize gameRenderSize(QSize view) const;
    void closeEvent(QCloseEvent* e) override;
    void log(const QString& line);
    void rebuildConsole();

    void navigateTo(const QString& dir);
    void updateBrowserDetails(const QModelIndex& index);
    void updateBrowserFilter();

    // Render thread and its frame pipeline. At most one frame is in flight; while it renders,
    // further refreshes only accumulate time and mark a new frame as wanted.
    RenderWorker* worker_ = nullptr;
    QThread thread_;
    bool modelReady_ = false;
    bool frameInFlight_ = false;
    bool renderWanted_ = false;
    float pendingDt_ = 0.0f;
    QImage lastFrame_;
    QSize renderSize_{960, 540};   // frame size requested from the engine for the active view

    // Camera and render state. Only the GUI thread touches these; the worker gets copies.
    ukiyo::OrbitCamera gameCam_;
    ukiyo::RenderSettings settings_;
    std::vector<std::function<void()>> settingSyncers_;   // re-read controls from settings_

    // Playback.
    QTimer timer_;
    QElapsedTimer clock_;
    bool playing_ = false;
    engine::ProjectSettings projSettings_;   // ProjectSettings/ProjectSettings.json, see the project settings pages
    float time_ = 0.0f;
    bool pinTime_ = false;      // selftest: render at pinnedTime_ instead of time_ (time-dependent effects)
    float pinnedTime_ = 0.0f;
    float clipLength_ = 8.0f;

    // Frame statistics for the status bar.
    QElapsedTimer fpsClock_;
    int framesThisSecond_ = 0;
    double fps_ = 0.0;
    double lastRenderMs_ = 0.0;
    uint32_t lastDrawCalls_ = 0;
    uint32_t lastTriangles_ = 0;
    // Scene benchmark: the first frames are warm-up, then the render times and counts are averaged.
    bool benchActive_ = false;
    int benchWarmup_ = 0;
    std::vector<double> benchMs_;
    double benchDraws_ = 0.0;
    double benchTris_ = 0.0;
    QString lastBenchReport_;
    bool maximized_ = false;                 // Maximize on Play is in effect
    int savedTab_ = -1;                      // the central tab to return to on stop
    std::vector<std::pair<QPointer<QDockWidget>, bool>> savedDocks_;   // dock, visible before play

    // Scene snapshot (rest pose and hierarchy) provided by the render thread.
    QString projectRoot_;
    QString modelPath_;
    QString sceneName_;
    std::vector<NodeInfo> nodes_;
    int headNode_ = -1;
    float headY_ = 1.45f;

    // Scene document: the .ukscene file being edited and whether it has unsaved changes.
    QString scenePath_;
    bool sceneDirty_ = false;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QAction* pasteAction_ = nullptr;
    std::vector<QJsonObject> undoStack_;   // scene documents before each recorded edit
    std::vector<QJsonObject> redoStack_;   // documents that undo removed
    QString lastEditKey_;
    // Terrain editing state. The brush is tool state: it is not saved and is not an undo step.
    TerrainBrushSettings terrainBrush_;
    ukiyo::ErosionSettings terrainErosion_;
    int terrainFoliageSel_ = 0;                  // foliage type shown in the Inspector
    bool terrainStroke_ = false;                 // a drag with a terrain brush is under way
    std::vector<int> terrainStrokeIds_;          // records of the stroke in progress, in order
    std::map<int, TerrainEdit> terrainRecords_;  // undo records, kept while an undo or redo step refers to them
    int nextTerrainRecord_ = 1;
    QElapsedTimer terrainFlushTimer_;            // throttles GPU re-upload during a drag
    QElapsedTimer lastEditTimer_;
    QJsonObject instanceClipboard_;        // one model entry, as the scene file stores it
    bool hasInstanceClipboard_ = false;
    bool loadingScene_ = false;                // suppresses dirty tracking while a scene loads
    QByteArray factoryLayout_;                 // dock arrangement at startup, used by Default
    std::vector<ShortcutEntry> shortcuts_;
    PreferencesDialog* prefsDialog_ = nullptr;
    bool showColliders_ = false;             // Scene view overlay for physics colliders. Not saved.
    std::vector<glm::vec3> navPreview_;      // Scene view overlay: the last agent path preview. Not saved.
    bool bakeInFlight_ = false;              // a navmesh bake is running on the render thread
    QLabel* fpsLabel_ = nullptr;             // status bar
    QLabel* memLabel_ = nullptr;
    QLabel* sceneLabel_ = nullptr;
    QElapsedTimer memClock_;
    QDialog* findDlg_ = nullptr;
    QWidget* projectSettings_ = nullptr;

    // Central views.
    QTabWidget* viewTabs_ = nullptr;
    RenderView* viewport_ = nullptr;   // Scene
    RenderView* preview_ = nullptr;    // Game
    QWidget* gamePage_ = nullptr;      // Game tab: resolution bar above the view
    QComboBox* gameAspect_ = nullptr;
    QComboBox* gameRes_ = nullptr;
    QSlider* gameScale_ = nullptr;
    QLabel* gameScaleLabel_ = nullptr;
    QCheckBox* gameMaximize_ = nullptr;
    QCheckBox* gameGizmos_ = nullptr;
    QCheckBox* gameStats_ = nullptr;
    QCheckBox* gameMute_ = nullptr;

    // Toolbar.
    QPushButton* playBtn_ = nullptr;
    QPushButton* pauseBtn_ = nullptr;

    // Hierarchy and inspector.
    QTreeWidget* hierarchy_ = nullptr;
    QTreeWidgetItem* cameraItem_ = nullptr;
    QScrollArea* inspector_ = nullptr;
    std::vector<QDoubleSpinBox*> nodeSpins_;   // 3 translation, 3 rotation (deg), 3 scale
    int editNode_ = -1;
    std::vector<QDoubleSpinBox*> camSpins_;    // target xyz, distance, fov, near, far, yaw, pitch
    bool cameraSelected_ = false;
    ukiyo::OrbitCamera* inspectedCam_ = nullptr;   // camera shown in the Inspector (Main or Scene)

    // Scene view transform gizmos (scene_gizmo.h). Only model instances get gizmos.
    gizmo::Tool gizmoTool_ = gizmo::Tool::Move;
    gizmo::Part gizmoHover_ = gizmo::Part::None;
    gizmo::Part gizmoPart_ = gizmo::Part::None;   // the handle being dragged
    bool gizmoDragging_ = false;
    bool gizmoPushed_ = false;    // this drag pushed an undo step
    bool gizmoChanged_ = false;   // the drag has moved or turned the instance
    int gizmoIndex_ = -1;
    glm::vec3 gizmoOrigin0_{0.0f};   // position when the drag began
    float gizmoYaw0_ = 0.0f;
    float gizmoT0_ = 0.0f;           // axis parameter under the pointer when the drag began
    float gizmoTheta0_ = 0.0f;       // ring angle under the pointer when the drag began
    bool snapOn_ = false;
    float moveSnap_ = 0.25f;         // metres
    float rotateSnapDeg_ = 15.0f;
    QAction* gizmoMoveAction_ = nullptr;
    QAction* gizmoRotateAction_ = nullptr;
    QAction* gizmoScaleAction_ = nullptr;
    QAction* snapAction_ = nullptr;
    QComboBox* moveSnapBox_ = nullptr;
    QComboBox* rotateSnapBox_ = nullptr;
    std::vector<QDoubleSpinBox*> lightSpins_;      // Directional Light: direction xyz, intensity

    // Project browser: folder tree, thumbnail grid and details pane.
    QString browserDir_;
    QFileSystemModel* fsModel_ = nullptr;      // files and folders, for the grid
    QFileSystemModel* fsDirs_ = nullptr;       // folders only, for the tree
    QTreeView* fsTree_ = nullptr;
    QListView* fsList_ = nullptr;
    QLabel* fsPath_ = nullptr;
    QLineEdit* fsSearch_ = nullptr;
    QSlider* fsSize_ = nullptr;
    QToolButton* browserFilterBtn_ = nullptr;
    QStackedWidget* browserViews_ = nullptr;   // icon/list grid or details tree
    QTreeView* fsDetail_ = nullptr;
    QButtonGroup* browserViewGroup_ = nullptr;
    int browserView_ = 0;                      // 0 icons, 1 list, 2 details
    int browserKind_ = 0;     // 0 all, 1 models, 2 textures, 3 audio, 4 folders
    QLabel* detailPreview_ = nullptr;
    QLabel* detailTitle_ = nullptr;
    QLabel* detailText_ = nullptr;
    std::optional<ukiyo::WaterVolume> water_;       // the scene's water volume (item 16), none when empty
    MaterialInspector* materialInspector_ = nullptr;   // properties of a selected .ukmat
    QLabel* detailDeps_ = nullptr;                     // "Uses" and "Used by" lines in the details pane

    // Project browser file operations. The work is in the non-dialog functions, which the selftest calls;
    // the browser* functions ask the user first and report failures.
    void browserNewFolder(const QString& parentDir);
    void browserRename(const QString& path);
    void browserDelete(const QString& path);
    void browserImport(const QString& destDir);
    void browserContextMenu(QAbstractItemView* view, const QPoint& pos);
    void browserDrop(QWidget* viewport, const QPoint& pos, const QStringList& paths);
    bool browserKey(QAbstractItemView* view, int key, bool perform);
    QString browserPathAt(QAbstractItemView* view, const QModelIndex& index) const;
    void refreshBrowser();
    assetops::Result createBrowserFolder(const QString& parentDir, const QString& name);
    assetops::Result renameBrowserItem(const QString& path, const QString& newName);
    assetops::Result removeBrowserItem(const QString& path);
    assetops::Result importBrowserFiles(const QStringList& sources, const QString& destDir);
    bool assetInOpenScene(const QString& path);
    void revealInFileManager(const QString& path);

    // Animation window.
    QComboBox* clipBox_ = nullptr;
    AnimationPanel* animPanel_ = nullptr;
    AnimGraph* animGraph_ = nullptr;
    QDockWidget* projectDock_ = nullptr;   // the Project dock, so the console can show a file in it

    // Console.
    QListWidget* console_ = nullptr;
    QLineEdit* consoleSearch_ = nullptr;
    QToolButton* infoBtn_ = nullptr;
    QToolButton* warnBtn_ = nullptr;
    QToolButton* errorBtn_ = nullptr;
    QToolButton* collapseBtn_ = nullptr;   // fold consecutive repeats into one line with a count
    std::vector<LogEntry> logs_;
    QString consoleSourceOf(const QString& line) const;   // project file named in a log line, or empty
    void openConsoleSource(const QString& path);          // reveal an asset in the Project browser
    void copyConsoleSelection();
    void showConsoleMenu(const QPoint& pos);

    // Status bar.
    QLabel* stateLabel_ = nullptr;
    QLabel* statsLabel_ = nullptr;
};

} // namespace editor
