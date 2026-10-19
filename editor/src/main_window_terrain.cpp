// Terrain editing in the editor (item 18, steps 4 to 6): brush strokes and their undo records, erosion, the terrain
// part of the Inspector, and the self-test for them. It lives apart from main_window.cpp, which is too large to compile
// in the memory this machine has.

#include "main_window.h"

#include "inspector_ui.h"
#include "render_worker.h"
#include "scene_file.h"

#include <QCoreApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStatusBar>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>

namespace editor {

// A stroke is one undo step. Its records are listed under kTerrainUndoKey in the snapshot taken before it.

bool MainWindow::terrainPress(QPointF s) {
    const gizmo::Ray r = gizmo::screenRay(gameCam_, viewport_->width(), viewport_->height(), s);
    int index = -1;
    glm::vec3 hit(0.0f);
    runOnWorker([&] { index = worker_->pickTerrain(r.origin, r.dir, &hit); });
    if (index < 0) return false;        // off the terrain: the press goes on to selection
    if (!beginEdit(QString())) return false;
    terrainStrokeIds_.clear();
    terrainStroke_ = true;
    terrainFlushTimer_.restart();
    if (!terrainStrokeDab(s)) {         // the dab failed: leave no empty undo step
        terrainStroke_ = false;
        undoStack_.pop_back();
        updateEditActions();
        return false;
    }
    return true;
}

bool MainWindow::terrainStrokeDab(QPointF s) {
    const gizmo::Ray r = gizmo::screenRay(gameCam_, viewport_->width(), viewport_->height(), s);
    int index = -1;
    glm::vec3 hit(0.0f);
    TerrainEdit e;
    QString err;
    bool ok = false;
    runOnWorker([&] {
        index = worker_->pickTerrain(r.origin, r.dir, &hit);
        if (index >= 0) ok = worker_->terrainDab(index, hit, terrainBrush_, &e, &err);
    });
    if (index < 0) return false;        // the pointer is off the tile: nothing to paint
    if (!ok) {
        log("error: " + err);
        return false;
    }

    // A dab joins the stroke's last record when it changes the same thing: a heightmap, or one mask channel.
    int id = -1;
    if (!terrainStrokeIds_.empty()) {
        const auto last = terrainRecords_.find(terrainStrokeIds_.back());
        if (last != terrainRecords_.end() && last->second.path == e.path && last->second.target == e.target
            && last->second.channel == e.channel) {
            id = last->first;
        }
    }
    if (id < 0) {
        id = nextTerrainRecord_++;
        TerrainEdit rec;
        rec.path = e.path;
        rec.target = e.target;
        rec.channel = e.channel;
        terrainRecords_[id] = rec;
        terrainStrokeIds_.push_back(id);
        QJsonArray tags;
        for (int t : terrainStrokeIds_) tags.append(t);
        if (!undoStack_.empty()) undoStack_.back()[kTerrainUndoKey] = tags;
    }
    TerrainEdit& rec = terrainRecords_[id];
    rec.createdMask = rec.createdMask || e.createdMask;
    ukiyo::mergeHeightEdits(rec.heights, e.heights);
    ukiyo::mergePaintEdits(rec.paint, e.paint);

    // The GPU copy follows the stroke at most every 300 ms. Release flushes the rest.
    if (terrainFlushTimer_.elapsed() >= 300) {
        runOnWorker([&] { worker_->flushTerrainUploads(); });
        terrainFlushTimer_.restart();
        viewport_->update();
    }
    return true;
}

void MainWindow::terrainRelease() {
    terrainStroke_ = false;
    runOnWorker([&] { worker_->flushTerrainUploads(); });
    terrainStrokeIds_.clear();
    markDirty();
    pruneTerrainRecords();
    updateEditActions();
    viewport_->update();
}

bool MainWindow::applyTerrainRecord(int id, bool useAfter) {
    const auto it = terrainRecords_.find(id);
    if (it == terrainRecords_.end()) return false;
    QString err;
    bool ok = false;
    runOnWorker([&] { ok = worker_->applyTerrainEdit(it->second, useAfter, false, &err); });
    if (!ok) log("error: " + err);
    return ok;
}

// Drops records that no undo or redo step, and no stroke in progress, refers to.
void MainWindow::pruneTerrainRecords() {
    std::set<int> keep(terrainStrokeIds_.begin(), terrainStrokeIds_.end());
    auto collect = [&keep](const std::vector<QJsonObject>& stack) {
        for (const QJsonObject& doc : stack) {
            for (const QJsonValue& v : doc.value(kTerrainUndoKey).toArray()) keep.insert(v.toInt());
        }
    };
    collect(undoStack_);
    collect(redoStack_);
    for (auto it = terrainRecords_.begin(); it != terrainRecords_.end();) {
        it = keep.count(it->first) ? std::next(it) : terrainRecords_.erase(it);
    }
}

// Erodes the whole tile in one blocking pass (editor only, baked into the heightmap on save). One undo step.
void MainWindow::runErosion(int index) {
    std::vector<InstanceInfo> now;
    runOnWorker([&] { now = worker_->instances(); });
    if (index < 0 || index >= static_cast<int>(now.size()) || !now[index].terrain) return;
    const bool pushed = beginEdit(QString());
    if (!pushed) return;
    statusBar()->showMessage(QStringLiteral("Eroding the terrain. The editor waits until it is done."));
    QCoreApplication::processEvents();
    TerrainEdit e;
    QString err;
    bool ok = false;
    runOnWorker([&] { ok = worker_->terrainErode(index, terrainErosion_, &e, &err); });
    if (!ok) {
        undoStack_.pop_back();
        updateEditActions();
        log("error: " + err);
        return;
    }
    const int id = nextTerrainRecord_++;
    terrainRecords_[id] = e;
    QJsonArray tags;
    tags.append(id);
    undoStack_.back()[kTerrainUndoKey] = tags;
    markDirty();
    pruneTerrainRecords();
    updateEditActions();
    statusBar()->showMessage(QStringLiteral("Eroded %1 samples.").arg(e.heights.size()), 4000);
}

// Replaces the terrain settings of one instance (layers, foliage) as one undo step.
void MainWindow::applyTerrainSettings(int index, const ukiyo::TerrainSettings& ts, const QString& key) {
    const bool pushed = beginEdit(key);
    QString err;
    bool ok = false;
    runOnWorker([&] { ok = worker_->setInstanceTerrain(index, ts, &err); });
    if (!ok) {
        if (pushed) undoStack_.pop_back();
        updateEditActions();
        log("error: " + err);
        return;
    }
    markDirty();
    updateEditActions();
}

// The terrain part of the Inspector: the brush, erosion, layers, and foliage types. Brush settings are tool
// state. They are not saved and are not undo steps.
void MainWindow::addTerrainInspector(QVBoxLayout* layout, int index, const ukiyo::TerrainSettings& ts) {
    QStringList layerNames, typeNames;
    for (const ukiyo::TerrainLayer& l : ts.layers) layerNames << QString::fromStdString(l.name);
    for (const ukiyo::FoliageType& f : ts.foliage) typeNames << QString::fromStdString(f.name);

    // Brush
    QFormLayout* brush = addSection(layout, QStringLiteral("Terrain Brush"));
    QLabel* hint = new QLabel(QStringLiteral("Hold the mouse on the terrain and drag. Each stroke is one undo step."));
    hint->setWordWrap(true);
    brush->addRow(hint);

    QComboBox* tool = new QComboBox;
    tool->addItems({QStringLiteral("Off (select objects)"), QStringLiteral("Noise"), QStringLiteral("Terrace"),
                    QStringLiteral("Stamp"), QStringLiteral("Layer mask"), QStringLiteral("Foliage density")});
    tool->setCurrentIndex(static_cast<int>(terrainBrush_.tool));
    brush->addRow(QStringLiteral("Tool"), tool);

    QComboBox* channel = new QComboBox;
    QComboBox* mode = new QComboBox;
    brush->addRow(QStringLiteral("Layer or type"), channel);
    brush->addRow(QStringLiteral("Mode"), mode);

    auto fillChannel = [this, channel, mode, layerNames, typeNames] {
        const TerrainTool t = terrainBrush_.tool;
        const bool mask = t == TerrainTool::LayerMask;
        const bool dens = t == TerrainTool::Density;
        QSignalBlocker b1(channel), b2(mode);
        channel->clear();
        mode->clear();
        if (mask) {
            channel->addItems(layerNames);
            mode->addItems({QStringLiteral("Allow"), QStringLiteral("Forbid")});
        } else if (dens) {
            channel->addItems(typeNames);
            mode->addItems({QStringLiteral("Plant"), QStringLiteral("Clear")});
        }
        channel->setEnabled(mask || dens);
        mode->setEnabled(mask || dens);
        const int last = std::max(0, channel->count() - 1);
        terrainBrush_.channel = std::clamp(terrainBrush_.channel, 0, last);
        channel->setCurrentIndex(terrainBrush_.channel);
        mode->setCurrentIndex(terrainBrush_.add ? 0 : 1);
    };

    auto makeSpin = [](double lo, double hi, double step, double value, const QString& suffix, int decimals) {
        QDoubleSpinBox* b = new QDoubleSpinBox;
        b->setRange(lo, hi);
        b->setSingleStep(step);
        b->setDecimals(decimals);
        b->setValue(value);
        b->setSuffix(suffix);
        return b;
    };
    QDoubleSpinBox* radius = makeSpin(0.5, 500.0, 0.5, terrainBrush_.radius, QStringLiteral(" m"), 1);
    QDoubleSpinBox* falloff = makeSpin(0.0, 1.0, 0.05, terrainBrush_.falloff, QString(), 2);
    QDoubleSpinBox* strength = makeSpin(-1.0, 1.0, 0.05, terrainBrush_.strength, QString(), 2);
    QDoubleSpinBox* amplitude = makeSpin(0.0, 50.0, 0.05, terrainBrush_.amplitude, QStringLiteral(" m"), 2);
    QDoubleSpinBox* wavelength = makeSpin(2.0, 1000.0, 1.0, terrainBrush_.wavelength, QStringLiteral(" m"), 1);
    QSpinBox* steps = new QSpinBox;
    steps->setRange(2, 32);
    steps->setValue(static_cast<int>(terrainBrush_.steps));
    brush->addRow(QStringLiteral("Radius"), radius);
    brush->addRow(QStringLiteral("Edge softness"), falloff);
    brush->addRow(QStringLiteral("Strength"), strength);
    brush->addRow(QStringLiteral("Height per dab"), amplitude);
    brush->addRow(QStringLiteral("Feature size"), wavelength);
    brush->addRow(QStringLiteral("Terrace steps"), steps);

    QLineEdit* stamp = new QLineEdit(terrainBrush_.stampFile);
    stamp->setPlaceholderText(QStringLiteral("built-in dome"));
    QPushButton* browse = new QPushButton(QStringLiteral("Browse..."));
    QWidget* stampRow = new QWidget;
    QHBoxLayout* sh = new QHBoxLayout(stampRow);
    sh->setContentsMargins(0, 0, 0, 0);
    sh->addWidget(stamp, 1);
    sh->addWidget(browse);
    brush->addRow(QStringLiteral("Stamp image"), stampRow);

    auto updateEnabled = [=] {
        const TerrainTool t = terrainBrush_.tool;
        const bool on = t != TerrainTool::None;
        radius->setEnabled(on);
        falloff->setEnabled(on);
        strength->setEnabled(t == TerrainTool::Noise || t == TerrainTool::Terrace || t == TerrainTool::Stamp);
        amplitude->setEnabled(t == TerrainTool::Noise || t == TerrainTool::Stamp);
        wavelength->setEnabled(t == TerrainTool::Noise);
        steps->setEnabled(t == TerrainTool::Terrace);
        stampRow->setEnabled(t == TerrainTool::Stamp);
    };
    fillChannel();
    updateEnabled();

    connect(tool, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, fillChannel, updateEnabled](int i) {
        terrainBrush_.tool = static_cast<TerrainTool>(i);
        fillChannel();
        updateEnabled();
    });
    connect(channel, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) {
        if (i >= 0) terrainBrush_.channel = i;
    });
    connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) {
        terrainBrush_.add = i == 0;
    });
    connect(radius, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double v) { terrainBrush_.radius = static_cast<float>(v); });
    connect(falloff, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double v) { terrainBrush_.falloff = static_cast<float>(v); });
    connect(strength, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double v) { terrainBrush_.strength = static_cast<float>(v); });
    connect(amplitude, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double v) { terrainBrush_.amplitude = static_cast<float>(v); });
    connect(wavelength, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double v) { terrainBrush_.wavelength = static_cast<float>(v); });
    connect(steps, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int v) { terrainBrush_.steps = static_cast<uint32_t>(v); });
    connect(stamp, &QLineEdit::editingFinished, this,
            [this, stamp] { terrainBrush_.stampFile = stamp->text(); });
    connect(browse, &QPushButton::clicked, this, [this, stamp] {
        const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("Stamp image"), projectRoot_,
                                                          QStringLiteral("Images (*.png *.jpg *.bmp)"));
        if (file.isEmpty()) return;
        stamp->setText(file);
        terrainBrush_.stampFile = file;
    });

    // Erosion
    QFormLayout* ero = addSection(layout, QStringLiteral("Erosion"));
    QSpinBox* drops = new QSpinBox;
    drops->setRange(1000, 2000000);
    drops->setSingleStep(10000);
    drops->setValue(static_cast<int>(terrainErosion_.droplets));
    ero->addRow(QStringLiteral("Droplets"), drops);
    connect(drops, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int v) { terrainErosion_.droplets = static_cast<uint32_t>(v); });
    QPushButton* erode = new QPushButton(uiIcon("tune", 16), QStringLiteral("Erode Tile"));
    erode->setToolTip(QStringLiteral("Runs on the whole tile as one undo step. The editor waits until it is done."));
    ero->addRow(erode);
    connect(erode, &QPushButton::clicked, this, [this, index] { runErosion(index); });

    // Layers: their bands are set in the scene file for now.
    QFormLayout* lay = addSection(layout, QStringLiteral("Layers"));
    for (const ukiyo::TerrainLayer& l : ts.layers) {
        const QString bands = QStringLiteral("%1 to %2 m, slope %3 to %4 deg")
                                  .arg(l.heightMin * ts.heightScale, 0, 'f', 1)
                                  .arg(l.heightMax * ts.heightScale, 0, 'f', 1)
                                  .arg(l.slopeMin, 0, 'f', 0)
                                  .arg(l.slopeMax, 0, 'f', 0);
        QLabel* text = new QLabel(bands);
        text->setWordWrap(true);
        lay->addRow(QString::fromStdString(l.name), text);
    }

    // Grass (item 19). The scatter, the distance bands and the wind. The layer masks and the two colours are set in
    // the scene file for now; every control here is one undo step, merged while it is being dragged.
    QFormLayout* gr = addSection(layout, QStringLiteral("Grass"));
    auto editGrass = [this, index](const QString& field, const std::function<void(ukiyo::GrassSettings&)>& change) {
        std::vector<InstanceInfo> now;
        runOnWorker([&] { now = worker_->instances(); });
        if (index < 0 || index >= static_cast<int>(now.size()) || !now[index].terrain) return;
        ukiyo::TerrainSettings next = *now[index].terrain;
        change(next.grass);
        applyTerrainSettings(index, next, QStringLiteral("terrain-grass:%1:%2").arg(index).arg(field));
    };
    QCheckBox* grassOn = new QCheckBox(QStringLiteral("Scatter grass on this tile"));
    grassOn->setChecked(ts.grass.enabled);
    gr->addRow(grassOn);
    connect(grassOn, &QCheckBox::toggled, this, [editGrass](bool on) {
        editGrass(QStringLiteral("enabled"), [on](ukiyo::GrassSettings& g) { g.enabled = on; });
    });
    auto grassNumber = [&](const QString& label, const QString& field, double lo, double hi, double step, int decimals,
                           double value, const QString& suffix,
                           std::function<void(ukiyo::GrassSettings&, double)> set) {
        QDoubleSpinBox* b = makeSpin(lo, hi, step, value, suffix, decimals);
        gr->addRow(label, b);
        connect(b, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [editGrass, field, set](double v) {
            editGrass(field, [set, v](ukiyo::GrassSettings& g) { set(g, v); });
        });
    };
    grassNumber(QStringLiteral("Density"), QStringLiteral("density"), 0.0, 8.0, 0.05, 2, ts.grass.density,
                QStringLiteral(" /m2"), [](ukiyo::GrassSettings& g, double v) { g.density = static_cast<float>(v); });
    grassNumber(QStringLiteral("Blade height"), QStringLiteral("height"), 0.05, 3.0, 0.01, 2, ts.grass.height,
                QStringLiteral(" m"), [](ukiyo::GrassSettings& g, double v) { g.height = static_cast<float>(v); });
    grassNumber(QStringLiteral("Scale from"), QStringLiteral("scaleMin"), 0.1, 4.0, 0.05, 2, ts.grass.scaleMin, QString(),
                [](ukiyo::GrassSettings& g, double v) {
                    g.scaleMin = static_cast<float>(v);
                    g.scaleMax = std::max(g.scaleMax, g.scaleMin);
                });
    grassNumber(QStringLiteral("Scale to"), QStringLiteral("scaleMax"), 0.1, 4.0, 0.05, 2, ts.grass.scaleMax, QString(),
                [](ukiyo::GrassSettings& g, double v) {
                    g.scaleMax = static_cast<float>(v);
                    g.scaleMin = std::min(g.scaleMin, g.scaleMax);
                });
    grassNumber(QStringLiteral("Slope limit"), QStringLiteral("slopeMax"), 0.0, 90.0, 1.0, 0, ts.grass.slopeMax,
                QStringLiteral(" deg"), [](ukiyo::GrassSettings& g, double v) { g.slopeMax = static_cast<float>(v); });
    grassNumber(QStringLiteral("Near band"), QStringLiteral("bandNear"), 0.0, 1000.0, 1.0, 0, ts.grass.bandNear,
                QStringLiteral(" m"), [](ukiyo::GrassSettings& g, double v) {
                    g.bandNear = static_cast<float>(v);
                    g.bandMid = std::max(g.bandMid, g.bandNear);
                    g.cullDistance = std::max(g.cullDistance, g.bandMid);
                });
    grassNumber(QStringLiteral("Half density at"), QStringLiteral("bandMid"), 0.0, 1000.0, 1.0, 0, ts.grass.bandMid,
                QStringLiteral(" m"), [](ukiyo::GrassSettings& g, double v) {
                    g.bandMid = static_cast<float>(v);
                    g.bandNear = std::min(g.bandNear, g.bandMid);
                    g.cullDistance = std::max(g.cullDistance, g.bandMid);
                });
    grassNumber(QStringLiteral("Cull distance"), QStringLiteral("cullDistance"), 1.0, 2000.0, 5.0, 0,
                ts.grass.cullDistance, QStringLiteral(" m"), [](ukiyo::GrassSettings& g, double v) {
                    g.cullDistance = static_cast<float>(v);
                    g.cullDistance = std::max(g.cullDistance, std::max(g.bandNear, g.bandMid));
                });
    grassNumber(QStringLiteral("Wind strength"), QStringLiteral("windStrength"), 0.0, 2.0, 0.01, 2, ts.grass.windStrength,
                QStringLiteral(" m"), [](ukiyo::GrassSettings& g, double v) { g.windStrength = static_cast<float>(v); });
    grassNumber(QStringLiteral("Wind frequency"), QStringLiteral("windFrequency"), 0.0, 4.0, 0.05, 2,
                ts.grass.windFrequency, QStringLiteral(" Hz"),
                [](ukiyo::GrassSettings& g, double v) { g.windFrequency = static_cast<float>(v); });
    QLineEdit* detail = new QLineEdit(QString::fromStdString(ts.grass.densityMap));
    detail->setPlaceholderText(QStringLiteral("uniform density"));
    QPushButton* detailBrowse = new QPushButton(QStringLiteral("Browse..."));
    QWidget* detailRow = new QWidget;
    QHBoxLayout* dh = new QHBoxLayout(detailRow);
    dh->setContentsMargins(0, 0, 0, 0);
    dh->addWidget(detail, 1);
    dh->addWidget(detailBrowse);
    gr->addRow(QStringLiteral("Density map"), detailRow);
    auto applyGrassDetail = [this, editGrass, detail] {
        const QString file = detail->text();
        editGrass(QStringLiteral("densityMap"), [file](ukiyo::GrassSettings& g) { g.densityMap = file.toStdString(); });
    };
    connect(detail, &QLineEdit::editingFinished, this, applyGrassDetail);
    connect(detailBrowse, &QPushButton::clicked, this, [this, detail, applyGrassDetail] {
        const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("Grass density map"), projectRoot_,
                                                          QStringLiteral("Images (*.png *.jpg *.bmp)"));
        if (file.isEmpty()) return;
        detail->setText(file);
        applyGrassDetail();
    });
    QLabel* grassHint = new QLabel(QStringLiteral("Blades thin out from %1 m and are gone by %2 m. A tile keeps at "
                                                  "most %3 of them, however dense the scatter.")
                                       .arg(ts.grass.bandMid, 0, 'f', 0)
                                       .arg(ts.grass.cullDistance, 0, 'f', 0)
                                       .arg(ukiyo::kGrassMaxInstances));
    grassHint->setWordWrap(true);
    gr->addRow(grassHint);

    // Foliage types
    QFormLayout* fol = addSection(layout, QStringLiteral("Foliage"));
    QComboBox* typeBox = new QComboBox;
    typeBox->addItems(typeNames);
    QPushButton* addType = new QPushButton(QStringLiteral("Add"));
    QPushButton* removeType = new QPushButton(QStringLiteral("Remove"));
    QWidget* typeRow = new QWidget;
    QHBoxLayout* th = new QHBoxLayout(typeRow);
    th->setContentsMargins(0, 0, 0, 0);
    th->addWidget(typeBox, 1);
    th->addWidget(addType);
    th->addWidget(removeType);
    fol->addRow(QStringLiteral("Type"), typeRow);
    removeType->setEnabled(!ts.foliage.empty());
    const int t = std::clamp(terrainFoliageSel_, 0, std::max(0, static_cast<int>(ts.foliage.size()) - 1));
    if (!ts.foliage.empty()) typeBox->setCurrentIndex(t);
    terrainFoliageSel_ = t;

    connect(typeBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, index](int i) {
        if (i < 0) return;
        terrainFoliageSel_ = i;
        QTimer::singleShot(0, this, [this, index] { showInstanceProperties(index); });
    });
    connect(addType, &QPushButton::clicked, this, [this, index] {
        std::vector<InstanceInfo> now;
        runOnWorker([&] { now = worker_->instances(); });
        if (index < 0 || index >= static_cast<int>(now.size()) || !now[index].terrain) return;
        ukiyo::TerrainSettings next = *now[index].terrain;
        if (next.foliage.size() >= static_cast<size_t>(ukiyo::kFoliageMaxTypes)) {
            statusBar()->showMessage(QStringLiteral("A terrain has up to %1 foliage types.").arg(ukiyo::kFoliageMaxTypes), 3000);
            return;
        }
        ukiyo::FoliageType f;
        f.name = "foliage " + std::to_string(next.foliage.size() + 1);
        f.seed = static_cast<uint32_t>(next.foliage.size() + 1);
        next.foliage.push_back(f);
        terrainFoliageSel_ = static_cast<int>(next.foliage.size()) - 1;
        applyTerrainSettings(index, next, QString());
        QTimer::singleShot(0, this, [this, index] { showInstanceProperties(index); });
    });
    connect(removeType, &QPushButton::clicked, this, [this, index] {
        std::vector<InstanceInfo> now;
        runOnWorker([&] { now = worker_->instances(); });
        if (index < 0 || index >= static_cast<int>(now.size()) || !now[index].terrain) return;
        ukiyo::TerrainSettings next = *now[index].terrain;
        if (terrainFoliageSel_ < 0 || terrainFoliageSel_ >= static_cast<int>(next.foliage.size())) return;
        next.foliage.erase(next.foliage.begin() + terrainFoliageSel_);
        terrainFoliageSel_ = 0;
        applyTerrainSettings(index, next, QString());
        QTimer::singleShot(0, this, [this, index] { showInstanceProperties(index); });
    });

    if (ts.foliage.empty()) {
        QLabel* none = new QLabel(QStringLiteral("No foliage. Add a type to scatter grass or trees."));
        none->setWordWrap(true);
        fol->addRow(none);
        return;
    }

    // Fields of the selected type. Each change is one undo step, merged while the field is being adjusted.
    const ukiyo::FoliageType ft = ts.foliage[t];
    auto editFoliage = [this, index, t](const QString& field, const std::function<void(ukiyo::FoliageType&)>& change) {
        std::vector<InstanceInfo> now;
        runOnWorker([&] { now = worker_->instances(); });
        if (index < 0 || index >= static_cast<int>(now.size()) || !now[index].terrain) return;
        ukiyo::TerrainSettings next = *now[index].terrain;
        if (t < 0 || t >= static_cast<int>(next.foliage.size())) return;
        change(next.foliage[t]);
        applyTerrainSettings(index, next, QStringLiteral("terrain-foliage:%1:%2:%3").arg(index).arg(t).arg(field));
    };

    QLineEdit* name = new QLineEdit(QString::fromStdString(ft.name));
    fol->addRow(QStringLiteral("Name"), name);
    connect(name, &QLineEdit::editingFinished, this, [editFoliage, name] {
        const std::string value = name->text().toStdString();
        editFoliage(QStringLiteral("name"), [value](ukiyo::FoliageType& f) { f.name = value; });
    });

    QComboBox* shape = new QComboBox;
    shape->addItems({QStringLiteral("Tuft (grass)"), QStringLiteral("Cone (tree)")});
    shape->setCurrentIndex(ft.shape == ukiyo::FoliageShape::Cone ? 1 : 0);
    fol->addRow(QStringLiteral("Shape"), shape);
    connect(shape, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [editFoliage](int i) {
        editFoliage(QStringLiteral("shape"),
                    [i](ukiyo::FoliageType& f) { f.shape = i == 1 ? ukiyo::FoliageShape::Cone : ukiyo::FoliageShape::Tuft; });
    });

    // A numeric field: `set` writes the value, and `fix` keeps a band's ends in order.
    auto numberField = [&](const QString& label, const QString& field, double lo, double hi, double step, int decimals,
                           double value, const QString& suffix,
                           std::function<void(ukiyo::FoliageType&, double)> set) {
        QDoubleSpinBox* b = makeSpin(lo, hi, step, value, suffix, decimals);
        fol->addRow(label, b);
        connect(b, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [editFoliage, field, set](double v) {
            editFoliage(field, [set, v](ukiyo::FoliageType& f) { set(f, v); });
        });
    };
    numberField(QStringLiteral("Density"), QStringLiteral("density"), 0.0, 1.0, 0.05, 2, ft.density, QString(),
                [](ukiyo::FoliageType& f, double v) { f.density = static_cast<float>(v); });
    numberField(QStringLiteral("Height from"), QStringLiteral("heightMin"), 0.0, 1.0, 0.01, 2, ft.heightMin, QString(),
                [](ukiyo::FoliageType& f, double v) {
                    f.heightMin = static_cast<float>(v);
                    f.heightMax = std::max(f.heightMax, f.heightMin);
                });
    numberField(QStringLiteral("Height to"), QStringLiteral("heightMax"), 0.0, 1.0, 0.01, 2, ft.heightMax, QString(),
                [](ukiyo::FoliageType& f, double v) {
                    f.heightMax = static_cast<float>(v);
                    f.heightMin = std::min(f.heightMin, f.heightMax);
                });
    numberField(QStringLiteral("Slope limit"), QStringLiteral("slopeMax"), 0.0, 90.0, 1.0, 0, ft.slopeMax,
                QStringLiteral(" deg"), [](ukiyo::FoliageType& f, double v) { f.slopeMax = static_cast<float>(v); });
    numberField(QStringLiteral("Scale from"), QStringLiteral("scaleMin"), 0.1, 5.0, 0.05, 2, ft.scaleMin, QString(),
                [](ukiyo::FoliageType& f, double v) {
                    f.scaleMin = static_cast<float>(v);
                    f.scaleMax = std::max(f.scaleMax, f.scaleMin);
                });
    numberField(QStringLiteral("Scale to"), QStringLiteral("scaleMax"), 0.1, 5.0, 0.05, 2, ft.scaleMax, QString(),
                [](ukiyo::FoliageType& f, double v) {
                    f.scaleMax = static_cast<float>(v);
                    f.scaleMin = std::min(f.scaleMin, f.scaleMax);
                });
    numberField(QStringLiteral("Fade starts"), QStringLiteral("fadeStart"), 0.0, 1000.0, 5.0, 0, ft.fadeStart,
                QStringLiteral(" m"), [](ukiyo::FoliageType& f, double v) {
                    f.fadeStart = static_cast<float>(v);
                    f.fadeEnd = std::max(f.fadeEnd, f.fadeStart);
                });
    numberField(QStringLiteral("Fade ends"), QStringLiteral("fadeEnd"), 0.0, 1000.0, 5.0, 0, ft.fadeEnd,
                QStringLiteral(" m"), [](ukiyo::FoliageType& f, double v) {
                    f.fadeEnd = static_cast<float>(v);
                    f.fadeStart = std::min(f.fadeStart, f.fadeEnd);
                });
    QSpinBox* seed = new QSpinBox;
    seed->setRange(1, 999999);
    seed->setValue(static_cast<int>(ft.seed));
    fol->addRow(QStringLiteral("Seed"), seed);
    connect(seed, QOverload<int>::of(&QSpinBox::valueChanged), this, [editFoliage](int v) {
        editFoliage(QStringLiteral("seed"), [v](ukiyo::FoliageType& f) { f.seed = static_cast<uint32_t>(v); });
    });
}

// The self-test for terrain editing (section 18). Every write goes to a temp copy of the fixture. tests/data is only
// read, and its hash is checked at the end.
void MainWindow::selfTestTerrainEdits(const std::function<void(bool, const QString&, const QString&)>& report) {
    // The runner's check takes an optional detail, which a std::function cannot default. Supply it here.
    auto check = [&report](bool ok, const QString& name, const QString& detail = QString()) { report(ok, name, detail); };
    // Every write goes to a temp copy of the fixture. tests/data is only read, and its hash is checked at the end.
    {
        auto fileHash = [](const QString& p) {
            QFile f(p);
            if (!f.open(QIODevice::ReadOnly)) return QByteArray();
            return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256);
        };
        const QString fixture = projectRoot_ + QStringLiteral("/tests/data/terrain_hills_257.u16");
        const QByteArray fixtureHash = fileHash(fixture);
        check(!fixtureHash.isEmpty(), "terrain edit: the fixture can be read", fixture);
        const QString dir = QDir::tempPath() + QStringLiteral("/ukiyo_selftest_edit");
        QDir(dir).removeRecursively();
        QDir().mkpath(dir);
        const QString raw = dir + QStringLiteral("/edit_hills_257.u16");
        const QString tmpS = dir + QStringLiteral("/edit_scene.ukscene");
        const QString mask0 = dir + QStringLiteral("/edit_hills_257_layers0.png");
        const QString density0 = dir + QStringLiteral("/edit_hills_257_density0.png");
        check(QFile::copy(fixture, raw), "terrain edit: the fixture is copied to a temp file", raw);
        const QString one = projectRoot_ + QStringLiteral("/renders/roundtrip_test.ukscene");
        check(readScene(one), "terrain edit: the round-trip scene opens");

        ukiyo::TerrainSettings ts;
        ts.heightmap = raw.toStdString();
        ts.size = 257;
        ts.cellSize = 1.0f;
        ts.heightScale = 24.0f;
        bool added = false, tOk = false;
        int made = -1;
        QString addErr, tErr;
        ModelInfo info2;
        const float origin[3] = {0.0f, 0.0f, 0.0f};
        runOnWorker([&] {
            added = scene_file::ensureEmptyModelFile(&addErr) && worker_->addModel(scene_file::emptyModelPath(), origin, 0.0f, &addErr, &info2, &made);
            if (added) {
                worker_->setInstanceName(made, QStringLiteral("Terrain"));
                tOk = worker_->setInstanceTerrain(made, ts, &tErr);
            }
        });
        check(added && tOk, "terrain edit: a tile is added from the temp copy", added ? tErr : addErr);

        // The tile's index can change when a scene is rebuilt (undo does this), so look it up each time.
        auto tileIndex = [this, raw]() -> int {
            std::vector<InstanceInfo> list;
            runOnWorker([&] { list = worker_->instances(); });
            for (size_t i = 0; i < list.size(); ++i) {
                if (list[i].terrain && QString::fromStdString(list[i].terrain->heightmap) == raw) return static_cast<int>(i);
            }
            return -1;
        };
        auto heightsOf = [this](int i) {
            std::vector<float> h;
            runOnWorker([&] { h = worker_->terrainHeights(i); });
            return h;
        };
        auto countDiff = [](const std::vector<float>& a, const std::vector<float>& b) -> long {
            if (a.size() != b.size()) return -1;
            long n = 0;
            for (size_t i = 0; i < a.size(); ++i) n += a[i] != b[i] ? 1 : 0;
            return n;
        };
        auto treesOf = [this](int i) {
            int n = 0;
            runOnWorker([&] { n = worker_->terrainFoliageCount(i); });
            return n;
        };

        // A camera over the tile, and the screen point of its centre. The test paints through the same calls as the view.
        const ukiyo::OrbitCamera savedCam = gameCam_;
        gameCam_.target = glm::vec3(128.0f, 6.0f, 128.0f);
        gameCam_.distance = 330.0f;
        gameCam_.yaw = 0.6f;
        gameCam_.pitch = 0.5f;
        const int vw = viewport_->width(), vh = viewport_->height();
        check(vw > 100 && vh > 100, "terrain edit: the view has a size to paint in", QString("%1 x %2").arg(vw).arg(vh));
        glm::vec3 hit0(0.0f);
        int hitIdx = -1;
        runOnWorker([&] { hitIdx = worker_->pickTerrain(glm::vec3(128.0f, 500.0f, 128.0f), glm::vec3(0.0f, -1.0f, 0.0f), &hit0); });
        QPointF centreS;
        const bool projected = gizmo::project(gameCam_, vw, vh, hit0, &centreS);
        check(hitIdx >= 0 && hitIdx == tileIndex() && projected, "terrain edit: a ray down onto the tile hits it",
              QString("index %1").arg(hitIdx));

        // A press off the terrain is left to selection: no undo step, and the redo history is kept.
        const int undo0 = static_cast<int>(undoStack_.size()), redo0 = static_cast<int>(redoStack_.size());
        gameCam_.target = glm::vec3(1000.0f, 6.0f, 1000.0f);
        terrainBrush_ = TerrainBrushSettings();
        terrainBrush_.tool = TerrainTool::Noise;
        const bool missed = terrainPress(QPointF(vw / 2.0, vh / 2.0));
        check(!missed && static_cast<int>(undoStack_.size()) == undo0 && static_cast<int>(redoStack_.size()) == redo0,
              "terrain edit: a press off the terrain is left to selection");
        gameCam_.target = glm::vec3(128.0f, 6.0f, 128.0f);

        // A noise stroke: press, drag, release. The whole stroke is one record, and one undo step.
        terrainBrush_ = TerrainBrushSettings();
        terrainBrush_.tool = TerrainTool::Noise;
        terrainBrush_.radius = 24.0f;
        terrainBrush_.falloff = 0.3f;
        terrainBrush_.strength = 1.0f;
        terrainBrush_.amplitude = 2.0f;
        terrainBrush_.wavelength = 30.0f;
        const std::vector<float> h0 = heightsOf(tileIndex());
        const bool pressed = terrainPress(centreS);
        gizmoDrag(QPointF(centreS.x() + 30.0, centreS.y()));
        gizmoDrag(QPointF(centreS.x() + 30.0, centreS.y() + 30.0));
        gizmoRelease();
        const std::vector<float> h1 = heightsOf(tileIndex());
        const long changed1 = countDiff(h0, h1);
        check(pressed && changed1 > 50, "terrain edit: a noise stroke changes the heights", QString("%1 samples").arg(changed1));
        const QJsonArray strokeTags = undoStack_.empty() ? QJsonArray() : undoStack_.back().value(kTerrainUndoKey).toArray();
        check(strokeTags.size() == 1 && !terrainStroke_, "terrain edit: a stroke is one undo step with one record",
              QString("%1 records").arg(strokeTags.size()));

        undoEdit();
        check(heightsOf(tileIndex()) == h0, "terrain edit: undo puts the heights back exactly");
        redoEdit();
        check(heightsOf(tileIndex()) == h1, "terrain edit: redo brings the stroke back exactly");

        // Erosion: one undo step over the whole tile. A small droplet count keeps the test quick.
        terrainErosion_.droplets = 3000;
        runErosion(tileIndex());
        const std::vector<float> h3 = heightsOf(tileIndex());
        const long changed3 = countDiff(h1, h3);
        check(changed3 > 0, "erosion: the heights change", QString("%1 samples").arg(changed3));
        undoEdit();
        check(heightsOf(tileIndex()) == h1, "erosion: undo restores the heights from before the erosion");

        // Layer mask: Forbid layer 1 in the middle. The first paint makes the mask, so undoing it removes the mask.
        QFile::remove(mask0);
        terrainBrush_ = TerrainBrushSettings();
        terrainBrush_.tool = TerrainTool::LayerMask;
        terrainBrush_.channel = 1;
        terrainBrush_.add = false;
        terrainBrush_.radius = 30.0f;
        terrainBrush_.falloff = 0.2f;
        terrainBrush_.strength = 1.0f;
        const bool forbidPressed = terrainPress(centreS);
        gizmoRelease();
        check(forbidPressed && writeScene(tmpS), "terrain layers: a Forbid stroke saves with the scene");
        const QImage m0(mask0);
        check(!m0.isNull() && m0.width() == 257 && qGreen(m0.pixel(128, 128)) < 128 && qGreen(m0.pixel(4, 4)) == 255,
              "terrain layers: Forbid removes the layer under the brush and leaves the rest allowed");
        undoEdit();
        check(writeScene(tmpS) && !QFile::exists(mask0), "terrain layers: undoing the first paint removes the mask file");

        // Density: with no density mask the cone type is uniform. The first paint makes a zero mask, so trees come only
        // where they are planted. Undoing that paint clears the mask again.
        QFile::remove(density0);
        ukiyo::FoliageType trees;
        trees.name = "trees";
        trees.shape = ukiyo::FoliageShape::Cone;
        trees.density = 0.2f;
        trees.slopeMax = 60.0f;
        trees.seed = 3;
        trees.fadeStart = 600.0f;
        trees.fadeEnd = 900.0f;
        const int ti = tileIndex();
        std::vector<InstanceInfo> cur;
        runOnWorker([&] { cur = worker_->instances(); });
        check(ti >= 0 && cur[ti].terrain.has_value(), "foliage density: the test tile is still in the scene",
              QString("index %1").arg(ti));
        if (ti < 0 || !cur[ti].terrain) return;
        ukiyo::TerrainSettings withTrees = *cur[ti].terrain;
        withTrees.foliage = {trees};
        bool setOk = false;
        QString setErr;
        runOnWorker([&] { setOk = worker_->setInstanceTerrain(ti, withTrees, &setErr); });
        check(setOk, "foliage: the test tile gets a tree type", setErr);
        const int uniform = treesOf(tileIndex());
        check(uniform > 0, "foliage density: with no mask the type is uniform", QString("%1 trees").arg(uniform));

        terrainBrush_ = TerrainBrushSettings();
        terrainBrush_.tool = TerrainTool::Density;
        terrainBrush_.channel = 0;
        terrainBrush_.add = true;
        terrainBrush_.radius = 30.0f;
        terrainBrush_.falloff = 0.2f;
        terrainBrush_.strength = 1.0f;
        terrainPress(centreS);
        gizmoRelease();
        const int planted = treesOf(tileIndex());
        check(planted > 0 && planted < uniform, "foliage density: planting limits the trees to the brush",
              QString("%1 of %2").arg(planted).arg(uniform));
        check(writeScene(tmpS) && QFile::exists(density0), "foliage density: the density mask is saved as an image");
        undoEdit();
        check(treesOf(tileIndex()) == uniform, "foliage density: undoing the first paint clears the density mask");
        check(writeScene(tmpS) && !QFile::exists(density0), "foliage density: the cleared mask's image is removed on save");

        // Heights and masks come back after a save and reopen. Heights are stored as 16-bit, so allow one step of rounding.
        const std::vector<float> beforeSave = heightsOf(tileIndex());
        check(beforeSave == h1, "terrain edit: the heights are the stroke's after the undos");
        check(writeScene(tmpS) && readScene(tmpS), "terrain edit: the edited scene saves and reopens");
        const std::vector<float> reopened = heightsOf(tileIndex());
        float worst = 0.0f;
        if (reopened.size() == beforeSave.size()) {
            for (size_t i = 0; i < reopened.size(); ++i) worst = std::max(worst, std::fabs(reopened[i] - beforeSave[i]));
        }
        check(reopened.size() == beforeSave.size() && worst <= 1.0f / 65535.0f,
              "terrain edit: the saved heights reopen within 16-bit rounding", QString::number(worst));

        // Clean up: the tile's terrain is removed, the camera and brush are restored, and the fixture is unchanged.
        const int ti2 = tileIndex();
        runOnWorker([&] { worker_->setInstanceTerrain(ti2, std::nullopt); });
        gameCam_ = savedCam;
        terrainBrush_ = TerrainBrushSettings();
        QDir(dir).removeRecursively();
        check(fileHash(fixture) == fixtureHash, "terrain edit: the test fixture in tests/data is untouched");
    }

}

} // namespace editor
