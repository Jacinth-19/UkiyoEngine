#include "anim_graph.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGraphicsItem>
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <map>

#include "anim_file.h"

using Cond = ukiyo::AnimStateMachine::Condition;
using ParamType = ukiyo::AnimParamDef::Type;

namespace {

constexpr double kNodeW = 150.0;
constexpr double kNodeH = 46.0;
const QString kAny = QStringLiteral("*");

const Cond::Op kOps[] = {Cond::Op::Greater, Cond::Op::Less, Cond::Op::Equal,
                         Cond::Op::IsTrue, Cond::Op::IsFalse, Cond::Op::Trigger};

QString opLabel(Cond::Op op) {
    switch (op) {
    case Cond::Op::Greater: return QStringLiteral(">");
    case Cond::Op::Less: return QStringLiteral("<");
    case Cond::Op::Equal: return QStringLiteral("=");
    case Cond::Op::IsTrue: return QStringLiteral("is true");
    case Cond::Op::IsFalse: return QStringLiteral("is false");
    case Cond::Op::Trigger: return QStringLiteral("triggered");
    }
    return QString();
}

bool opNeedsValue(Cond::Op op) {
    return op == Cond::Op::Greater || op == Cond::Op::Less || op == Cond::Op::Equal;
}

QString typeLabel(ParamType t) {
    switch (t) {
    case ParamType::Float: return QStringLiteral("Float");
    case ParamType::Bool: return QStringLiteral("Bool");
    case ParamType::Trigger: return QStringLiteral("Trigger");
    }
    return QString();
}

int stateIndex(const ukiyo::AnimComponent& g, const QString& name) {
    for (int i = 0; i < static_cast<int>(g.states.size()); ++i)
        if (QString::fromStdString(g.states[i].name) == name) return i;
    return -1;
}

QString condText(const Cond& c) {
    const QString p = QString::fromStdString(c.param);
    if (opNeedsValue(c.op)) return QString("%1 %2 %3").arg(p, opLabel(c.op)).arg(c.value, 0, 'g', 3);
    return QString("%1 %2").arg(p, opLabel(c.op));
}

QString transLabel(const ukiyo::AnimTransitionDef& t) {
    if (t.when.empty()) return QStringLiteral("no conditions");
    QStringList parts;
    for (const Cond& c : t.when) parts << condText(c);
    return parts.join(QStringLiteral(", "));
}

class TransitionItem;

// A state (or the "Any state" source of transitions drawn from every state).
class StateNode : public QGraphicsRectItem {
public:
    StateNode(const QString& name, const QString& clip, bool isDefault, bool any)
        : QGraphicsRectItem(-kNodeW / 2, -kNodeH / 2, kNodeW, kNodeH), name_(name), clip_(clip),
          default_(isDefault), any_(any) {
        setFlags(ItemIsSelectable | (any ? QGraphicsItem::GraphicsItemFlags() : (ItemIsMovable | ItemSendsGeometryChanges)));
        setZValue(2);
    }

    QString name_;
    QString clip_;
    bool default_;
    bool any_;

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;

    void paint(QPainter* p, const QStyleOptionGraphicsItem* opt, QWidget*) override {
        const bool sel = opt->state & QStyle::State_Selected;
        p->setRenderHint(QPainter::Antialiasing, true);
        const QColor fill = any_ ? QColor(0x2c, 0x2c, 0x34) : QColor(0x33, 0x3a, 0x48);
        const QColor edge = sel ? QColor(0xff, 0xa8, 0x40) : (default_ ? QColor(0x7f, 0xd6, 0xa0) : QColor(0x70, 0x78, 0x90));
        p->setPen(QPen(edge, (sel || default_) ? 2.2 : 1.2, any_ ? Qt::DashLine : Qt::SolidLine));
        p->setBrush(fill);
        p->drawRoundedRect(rect(), 8, 8);

        QFont f = p->font();
        f.setBold(true);
        p->setFont(f);
        p->setPen(QColor(0xe8, 0xe8, 0xf0));
        const QString title = any_ ? QStringLiteral("Any state") : name_;
        p->drawText(QRectF(-kNodeW / 2 + 8, -kNodeH / 2 + 4, kNodeW - 16, kNodeH / 2 - 4), Qt::AlignLeft | Qt::AlignVCenter,
                    title);
        f.setBold(false);
        f.setPointSizeF(std::max(7.0, f.pointSizeF() * 0.85));
        p->setFont(f);
        p->setPen(QColor(0xa8, 0xb0, 0xc0));
        const QString sub = any_ ? QStringLiteral("transitions from every state")
                                 : (clip_.isEmpty() ? QStringLiteral("idle loop") : clip_);
        p->drawText(QRectF(-kNodeW / 2 + 8, 2, kNodeW - 16, kNodeH / 2 - 6), Qt::AlignLeft | Qt::AlignVCenter, sub);
    }
};

// An arrow from one state to another, offset sideways when several transitions share the pair. It finds its
// states by name each time it moves, so it never keeps a pointer that a rebuild could leave dangling. Its label
// is a child item, so the two are always deleted together.
class TransitionItem : public QGraphicsPathItem {
public:
    TransitionItem(const QString& from, const QString& to, int index, double lane, const QString& text)
        : from_(from), to_(to), index_(index), lane_(lane) {
        setFlags(ItemIsSelectable);
        setZValue(1);
        label_ = new QGraphicsSimpleTextItem(text, this);
        label_->setBrush(QColor(0xc8, 0xd0, 0xe0));
        label_->setZValue(1);
    }

    // Recomputes the arrow from the current positions of its two states.
    void updatePath() {
        const StateNode* a = findNode(from_);
        const StateNode* b = findNode(to_);
        if (!a || !b) return;
        const QPointF pa = a->pos();
        const QPointF pb = b->pos();
        const QPointF d = pb - pa;
        const double len = std::hypot(d.x(), d.y());
        if (len < 1e-3) return;
        const QPointF u = d / len;
        const QPointF n(-u.y(), u.x());
        const double hw = kNodeW / 2 + 2, hh = kNodeH / 2 + 2;
        auto edge = [&](QPointF dir) {
            const double ax = std::abs(dir.x()), ay = std::abs(dir.y());
            const double tx = ax > 1e-6 ? hw / ax : 1e9;
            const double ty = ay > 1e-6 ? hh / ay : 1e9;
            return std::min(tx, ty);
        };
        const double off = lane_ * 18.0;
        const QPointF s = pa + u * edge(u) + n * off;
        const QPointF e = pb - u * edge(u) + n * off;
        QPainterPath path;
        path.moveTo(s);
        path.lineTo(e);
        const double L = 11.0, ang = 0.44;
        auto rot = [](QPointF v, double r) {
            return QPointF(v.x() * std::cos(r) - v.y() * std::sin(r), v.x() * std::sin(r) + v.y() * std::cos(r));
        };
        const QPointF back = -u;
        path.moveTo(e);
        path.lineTo(e + rot(back, ang) * L);
        path.moveTo(e);
        path.lineTo(e + rot(back, -ang) * L);
        setPath(path);
        setPen(penFor(isSelected()));
        label_->setPos((s + e) / 2 + n * 10);
    }

    int index() const { return index_; }

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override {
        if (change == ItemSelectedHasChanged) setPen(penFor(value.toBool()));
        return QGraphicsPathItem::itemChange(change, value);
    }

private:
    QPen penFor(bool selected) const {
        return QPen(selected ? QColor(0xff, 0xa8, 0x40) : QColor(0xa8, 0xb4, 0xd0), selected ? 2.4 : 2.0,
                    from_ == kAny ? Qt::DashLine : Qt::SolidLine);
    }

    const StateNode* findNode(const QString& name) const {
        if (!scene()) return nullptr;
        for (QGraphicsItem* it : scene()->items()) {
            if (auto* n = dynamic_cast<const StateNode*>(it)) {
                if (n->name_ == name) return n;
            }
        }
        return nullptr;
    }

    QString from_;
    QString to_;
    int index_;
    double lane_;
    QGraphicsSimpleTextItem* label_ = nullptr;
};

QVariant StateNode::itemChange(GraphicsItemChange change, const QVariant& value) {
    if (change == ItemPositionHasChanged && scene()) {
        for (QGraphicsItem* it : scene()->items()) {
            if (auto* t = dynamic_cast<TransitionItem*>(it)) t->updatePath();
        }
    }
    return QGraphicsRectItem::itemChange(change, value);
}

} // namespace

// ---------------------------------------------------------------- GraphView

GraphView::GraphView(QGraphicsScene* scene, QWidget* parent) : QGraphicsView(scene, parent) {
    setRenderHint(QPainter::Antialiasing, true);
    setBackgroundBrush(QColor(0x1d, 0x1d, 0x21));
    setDragMode(QGraphicsView::RubberBandDrag);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumHeight(260);
}

void GraphView::mousePressEvent(QMouseEvent* e) {
    setFocus();
    if (connecting && e->button() == Qt::LeftButton) {
        if (auto* n = dynamic_cast<StateNode*>(itemAt(e->pos()))) {
            emit nodeClicked(n->name_.isEmpty() ? kAny : (n->any_ ? kAny : n->name_));
            e->accept();
            return;
        }
    }
    QGraphicsView::mousePressEvent(e);
}

void GraphView::mouseReleaseEvent(QMouseEvent* e) {
    QGraphicsView::mouseReleaseEvent(e);
    if (e->button() == Qt::LeftButton) emit layoutMoved();
}

void GraphView::keyPressEvent(QKeyEvent* e) {
    const bool ctrl = e->modifiers() & Qt::ControlModifier;
    if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) {
        emit deleteRequested();
    } else if (ctrl && e->key() == Qt::Key_Z && !(e->modifiers() & Qt::ShiftModifier)) {
        emit undoRequested();
    } else if ((ctrl && e->key() == Qt::Key_Y) || (ctrl && e->key() == Qt::Key_Z)) {
        emit redoRequested();
    } else {
        QGraphicsView::keyPressEvent(e);
    }
}

bool GraphView::event(QEvent* e) {
    // Undo, redo and delete go to the graph, not the main window's shortcuts.
    if (e->type() == QEvent::ShortcutOverride) {
        auto* k = static_cast<QKeyEvent*>(e);
        const bool ctrl = k->modifiers() & Qt::ControlModifier;
        if (k->key() == Qt::Key_Delete || k->key() == Qt::Key_Backspace ||
            (ctrl && (k->key() == Qt::Key_Z || k->key() == Qt::Key_Y))) {
            e->accept();
            return true;
        }
    }
    return QGraphicsView::event(e);
}

// ---------------------------------------------------------------- AnimGraph: construction

AnimGraph::AnimGraph(QWidget* parent) : QWidget(parent) {
    scene_ = new QGraphicsScene(this);
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);

    // Left: toolbar, canvas, status.
    auto* left = new QVBoxLayout;
    auto* bar = new QHBoxLayout;
    customBox_ = new QCheckBox("Custom graph");
    customBox_->setToolTip("Off: the object uses the three-clip Idle, Walk and Run machine");
    connectBtn_ = new QPushButton("Connect");
    connectBtn_->setCheckable(true);
    connectBtn_->setToolTip("Click a state, then a target state, to add a transition");
    addStateBtn_ = new QPushButton("Add state");
    deleteBtn_ = new QPushButton("Delete");
    deleteBtn_->setEnabled(false);
    defaultBtn_ = new QPushButton("Set default");
    defaultBtn_->setEnabled(false);
    legacyBtn_ = new QPushButton("Start from Idle / Walk / Run");
    bar->addWidget(customBox_);
    bar->addWidget(connectBtn_);
    bar->addWidget(addStateBtn_);
    bar->addWidget(deleteBtn_);
    bar->addWidget(defaultBtn_);
    bar->addWidget(legacyBtn_);
    bar->addStretch(1);
    left->addLayout(bar);
    hint_ = new QLabel;
    hint_->setWordWrap(true);
    left->addWidget(hint_);
    view_ = new GraphView(scene_);
    left->addWidget(view_, 1);
    status_ = new QLabel;
    left->addWidget(status_);
    root->addLayout(left, 3);

    // Right: selection editor, parameters, problems, test.
    auto* right = new QVBoxLayout;

    auto* selBox = new QGroupBox("Selection");
    auto* selLayout = new QVBoxLayout(selBox);
    side_ = new QStackedWidget;
    side_->addWidget(new QLabel("Click a state or a transition to edit it. Drag states to arrange them."));

    auto* statePage = new QWidget;
    auto* sf = new QFormLayout(statePage);
    stateName_ = new QLineEdit;
    sf->addRow("Name", stateName_);
    stateClip_ = new QComboBox;
    sf->addRow("Clip", stateClip_);
    blendKind_ = new QComboBox;
    blendKind_->addItem("Single clip", static_cast<int>(ukiyo::BlendKind::None));
    blendKind_->addItem("1D blend", static_cast<int>(ukiyo::BlendKind::Linear1D));
    blendKind_->addItem("2D simple directional", static_cast<int>(ukiyo::BlendKind::SimpleDirectional2D));
    blendKind_->addItem("2D freeform cartesian", static_cast<int>(ukiyo::BlendKind::FreeformCartesian2D));
    blendKind_->addItem("2D freeform directional", static_cast<int>(ukiyo::BlendKind::FreeformDirectional2D));
    blendKind_->addItem("Direct", static_cast<int>(ukiyo::BlendKind::Direct));
    sf->addRow("Motion", blendKind_);
    paramX_ = new QComboBox;
    paramY_ = new QComboBox;
    sf->addRow("Parameter X", paramX_);
    sf->addRow("Parameter Y", paramY_);
    motions_ = new QTableWidget(0, 6);
    motions_->setHorizontalHeaderLabels({"Clip", "X", "Y", "Time scale", "Mirror", "Weight parameter"});
    motions_->verticalHeader()->setVisible(false);
    motions_->horizontalHeader()->setStretchLastSection(true);
    motions_->setSelectionBehavior(QAbstractItemView::SelectRows);
    motions_->setSelectionMode(QAbstractItemView::SingleSelection);
    motions_->setMinimumHeight(120);
    sf->addRow(motions_);
    auto* mbar = new QHBoxLayout;
    addMotionBtn_ = new QPushButton("Add motion");
    delMotionBtn_ = new QPushButton("Remove motion");
    autoMotionBtn_ = new QPushButton("Automate thresholds");
    mbar->addWidget(addMotionBtn_);
    mbar->addWidget(delMotionBtn_);
    mbar->addWidget(autoMotionBtn_);
    sf->addRow(mbar);
    blendHint_ = new QLabel;
    blendHint_->setWordWrap(true);
    sf->addRow(blendHint_);
    stateDefault_ = new QCheckBox("Default state (starts here)");
    sf->addRow(stateDefault_);
    side_->addWidget(statePage);

    auto* transPage = new QWidget;
    auto* tl = new QVBoxLayout(transPage);
    tl->setContentsMargins(0, 0, 0, 0);
    transRoute_ = new QLabel;
    tl->addWidget(transRoute_);
    auto* tf = new QFormLayout;
    fade_ = new QDoubleSpinBox;
    fade_->setRange(0.0, 10.0);
    fade_->setDecimals(2);
    fade_->setSingleStep(0.05);
    fade_->setSuffix(" s");
    tf->addRow("Crossfade", fade_);
    dwell_ = new QDoubleSpinBox;
    dwell_->setRange(0.0, 10.0);
    dwell_->setDecimals(2);
    dwell_->setSingleStep(0.05);
    dwell_->setSuffix(" s");
    dwell_->setToolTip("The state must have played at least this long before the transition can fire");
    tf->addRow("Minimum time in state", dwell_);
    exitCheck_ = new QCheckBox("Has exit time");
    exitCheck_->setToolTip("The transition may fire only when the source clip reaches the exit time");
    tf->addRow(exitCheck_);
    exitTime_ = new QDoubleSpinBox;
    exitTime_->setRange(0.0, 10.0);
    exitTime_->setDecimals(2);
    exitTime_->setSingleStep(0.05);
    exitTime_->setSuffix(" of clip");
    exitTime_->setToolTip("Below 1 it fires once per loop. At 1 or more it fires once, after that many loops.");
    tf->addRow("Exit time", exitTime_);
    fixed_ = new QCheckBox("Fixed duration (seconds)");
    fixed_->setToolTip("Off: the crossfade is a fraction of the length of the clip being left");
    tf->addRow(fixed_);
    offset_ = new QDoubleSpinBox;
    offset_->setRange(0.0, 1.0);
    offset_->setDecimals(2);
    offset_->setSingleStep(0.05);
    offset_->setToolTip("Where in the target clip playback starts. 0 = start, 0.5 = halfway.");
    tf->addRow("Start target at", offset_);
    selfCheck_ = new QCheckBox("Can transition to self");
    selfCheck_->setToolTip("Lets an Any State transition fire while already in its target state");
    tf->addRow(selfCheck_);
    tl->addLayout(tf);
    conds_ = new QTableWidget(0, 3);
    conds_->setHorizontalHeaderLabels({"Parameter", "Test", "Value"});
    conds_->horizontalHeader()->setStretchLastSection(true);
    conds_->verticalHeader()->setVisible(false);
    conds_->setMinimumHeight(110);
    tl->addWidget(conds_);
    auto* cbar = new QHBoxLayout;
    addCondBtn_ = new QPushButton("Add condition");
    delCondBtn_ = new QPushButton("Remove condition");
    deleteTransBtn_ = new QPushButton("Delete transition");
    cbar->addWidget(addCondBtn_);
    cbar->addWidget(delCondBtn_);
    cbar->addWidget(deleteTransBtn_);
    tl->addLayout(cbar);
    side_->addWidget(transPage);
    selLayout->addWidget(side_);
    right->addWidget(selBox);

    auto* parBox = new QGroupBox("Parameters");
    auto* pl = new QVBoxLayout(parBox);
    params_ = new QTableWidget(0, 3);
    params_->setHorizontalHeaderLabels({"Name", "Type", "Start"});
    params_->horizontalHeader()->setStretchLastSection(true);
    params_->verticalHeader()->setVisible(false);
    params_->setMinimumHeight(110);
    pl->addWidget(params_);
    auto* pbar = new QHBoxLayout;
    addParamBtn_ = new QPushButton("Add parameter");
    delParamBtn_ = new QPushButton("Remove parameter");
    pbar->addWidget(addParamBtn_);
    pbar->addWidget(delParamBtn_);
    pl->addLayout(pbar);
    right->addWidget(parBox);

    auto* probBox = new QGroupBox("Checks");
    auto* prl = new QVBoxLayout(probBox);
    problems_ = new QListWidget;
    problems_->setMinimumHeight(70);
    prl->addWidget(problems_);
    right->addWidget(probBox);

    auto* testGroup = new QGroupBox("Test");
    testBox_ = new QWidget;
    auto* tbl = new QVBoxLayout(testBox_);
    tbl->setContentsMargins(0, 0, 0, 0);
    testStateLabel_ = new QLabel;
    tbl->addWidget(testStateLabel_);
    auto* tbar = new QHBoxLayout;
    auto* advance = new QPushButton("Advance 1 s");
    auto* restart = new QPushButton("Restart");
    tbar->addWidget(advance);
    tbar->addWidget(restart);
    tbl->addLayout(tbar);
    testLog_ = new QListWidget;
    testLog_->setMinimumHeight(60);
    tbl->addWidget(testLog_);
    auto* tg = new QVBoxLayout(testGroup);
    tg->addWidget(testBox_);
    tg->addWidget(new QLabel("Setting a value restarts nothing. Editing the graph restarts this test."));
    right->addWidget(testGroup);
    right->addStretch(1);

    auto* holder = new QWidget;
    holder->setLayout(right);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setWidget(holder);
    scroll->setFrameShape(QFrame::NoFrame);
    root->addWidget(scroll, 2);

    // Wiring.
    connect(customBox_, &QCheckBox::toggled, this, [this](bool on) {
        if (refreshing_) return;
        ukiyo::AnimComponent next = graph_;
        next.custom = on;
        commit(next, on ? "Custom graph on" : "Custom graph off");
    });
    connect(connectBtn_, &QPushButton::toggled, this, [this](bool on) {
        view_->connecting = on;
        connectFrom_.clear();
        status_->setText(on ? "Click the source state, then the target state." : "");
    });
    connect(addStateBtn_, &QPushButton::clicked, this, [this] { addState(QString(), QString()); });
    connect(deleteBtn_, &QPushButton::clicked, this, [this] { onDeleteKey(); });
    connect(defaultBtn_, &QPushButton::clicked, this, [this] {
        if (!selState_.isEmpty()) setDefault(selState_);
    });
    connect(legacyBtn_, &QPushButton::clicked, this, [this] { createFromLegacy(); });
    connect(view_, &GraphView::nodeClicked, this, &AnimGraph::onNodePicked);
    connect(view_, &GraphView::layoutMoved, this, &AnimGraph::onLayoutMoved);
    connect(view_, &GraphView::deleteRequested, this, &AnimGraph::onDeleteKey);
    connect(view_, &GraphView::undoRequested, this, [this] { undo(); });
    connect(view_, &GraphView::redoRequested, this, [this] { redo(); });
    connect(scene_, &QGraphicsScene::selectionChanged, this, &AnimGraph::onSceneSelection);

    connect(stateName_, &QLineEdit::editingFinished, this, &AnimGraph::onRenameEdited);
    connect(stateClip_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        if (refreshing_) return;
        const int i = stateIndex(graph_, selState_);
        if (i < 0) return;
        ukiyo::AnimComponent next = graph_;
        next.states[i].clip = stateClip_->currentData().toString().toStdString();
        commit(next, "Changed clip", false);
    });
    connect(stateDefault_, &QCheckBox::toggled, this, [this](bool on) {
        if (refreshing_ || selState_.isEmpty()) return;
        setDefault(on ? selState_ : QString());
    });
    connect(blendKind_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        if (!refreshing_) writeBlendKind();
    });
    connect(paramX_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        if (!refreshing_) writeBlendParams();
    });
    connect(paramY_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        if (!refreshing_) writeBlendParams();
    });
    connect(autoMotionBtn_, &QPushButton::clicked, this, [this] {
        const int i = stateIndex(graph_, selState_);
        if (i < 0 || graph_.states[i].blend != ukiyo::BlendKind::Linear1D || graph_.states[i].motions.size() < 2) return;
        ukiyo::AnimComponent next = graph_;
        next.states[i].motions = ukiyo::spreadThresholds(next.states[i].motions);
        commit(next, "Automated thresholds", true);
    });
    connect(addMotionBtn_, &QPushButton::clicked, this, [this] {
        const int i = stateIndex(graph_, selState_);
        if (i < 0) return;
        ukiyo::AnimComponent next = graph_;
        ukiyo::BlendMotion m;
        m.clip = clips_.isEmpty() ? std::string() : clips_.first().toStdString();
        // A new motion gets a placeholder position that is easy to tell apart. The user edits it next.
        const int n = static_cast<int>(next.states[i].motions.size());
        m.x = static_cast<float>(n);
        m.y = next.states[i].blend == ukiyo::BlendKind::Linear1D ? 0.0f : 1.0f;
        next.states[i].motions.push_back(m);
        commit(next, "Added a motion", true);
    });
    connect(delMotionBtn_, &QPushButton::clicked, this, [this] {
        const int i = stateIndex(graph_, selState_);
        const int r = motions_->currentRow();
        if (i < 0 || r < 0 || r >= static_cast<int>(graph_.states[i].motions.size())) return;
        ukiyo::AnimComponent next = graph_;
        next.states[i].motions.erase(next.states[i].motions.begin() + r);
        commit(next, "Removed a motion", true);
    });
    auto timing = [this] {
        if (refreshing_ || selTrans_ < 0 || selTrans_ >= static_cast<int>(graph_.transitions.size())) return;
        ukiyo::AnimComponent next = graph_;
        next.transitions[selTrans_].fade = static_cast<float>(fade_->value());
        next.transitions[selTrans_].minDwell = static_cast<float>(dwell_->value());
        auto& o = next.transitions[selTrans_].opts;
        o.hasExitTime = exitCheck_->isChecked();
        o.exitTime = static_cast<float>(exitTime_->value());
        o.fixedDuration = fixed_->isChecked();
        o.offset = static_cast<float>(offset_->value());
        o.toSelf = selfCheck_->isChecked();
        commit(next, "Changed transition timing", false);
    };
    connect(fade_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [timing](double) { timing(); });
    connect(dwell_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [timing](double) { timing(); });
    connect(exitTime_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [timing](double) { timing(); });
    connect(offset_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [timing](double) { timing(); });
    connect(exitCheck_, &QCheckBox::toggled, this, [timing](bool) { timing(); });
    connect(fixed_, &QCheckBox::toggled, this, [timing](bool) { timing(); });
    connect(selfCheck_, &QCheckBox::toggled, this, [timing](bool) { timing(); });
    connect(addCondBtn_, &QPushButton::clicked, this, [this] {
        if (selTrans_ < 0 || selTrans_ >= static_cast<int>(graph_.transitions.size())) return;
        ukiyo::AnimComponent next = graph_;
        Cond c;
        c.param = paramNames().isEmpty() ? QStringLiteral("speed").toStdString() : paramNames().first().toStdString();
        c.op = Cond::Op::Greater;
        c.value = 0.0f;
        next.transitions[selTrans_].when.push_back(c);
        commit(next, "Added condition");
    });
    connect(delCondBtn_, &QPushButton::clicked, this, [this] {
        if (selTrans_ < 0 || selTrans_ >= static_cast<int>(graph_.transitions.size())) return;
        ukiyo::AnimComponent next = graph_;
        auto& when = next.transitions[selTrans_].when;
        if (when.empty()) return;
        int row = conds_->currentRow();
        if (row < 0 || row >= static_cast<int>(when.size())) row = static_cast<int>(when.size()) - 1;
        when.erase(when.begin() + row);
        commit(next, "Removed condition");
    });
    connect(deleteTransBtn_, &QPushButton::clicked, this, [this] { deleteTransition(selTrans_); });
    connect(addParamBtn_, &QPushButton::clicked, this, [this] {
        QString name = QStringLiteral("param");
        for (int k = 2; paramNames().contains(name); ++k) name = QStringLiteral("param%1").arg(k);
        addParam(name, ParamType::Float, 0.0f);
    });
    connect(delParamBtn_, &QPushButton::clicked, this, [this] {
        if (graph_.params.empty()) return;
        int row = params_->currentRow();
        if (row < 0 || row >= static_cast<int>(graph_.params.size())) row = static_cast<int>(graph_.params.size()) - 1;
        ukiyo::AnimComponent next = graph_;
        next.params.erase(next.params.begin() + row);
        commit(next, "Removed parameter");
    });
    connect(advance, &QPushButton::clicked, this, [this] { testAdvance(1.0f); });
    connect(restart, &QPushButton::clicked, this, [this] { rebuildTestMachine(); });

    setInstance(-1, ukiyo::AnimComponent{});
}

AnimGraph::~AnimGraph() {
    // The scene is deleted with this object. Its clear() reports selection changes, which must not
    // rebuild the side pane while the widgets are going away.
    refreshing_ = true;
    if (scene_) scene_->blockSignals(true);
}

// ---------------------------------------------------------------- data in and out

void AnimGraph::setInstance(int instance, const ukiyo::AnimComponent& anim) {
    instance_ = instance;
    graph_ = anim;
    undo_.clear();
    redo_.clear();
    selState_.clear();
    selTrans_ = -1;
    connectFrom_.clear();
    if (connectBtn_->isChecked()) connectBtn_->setChecked(false);
    refreshAll(true);
}

void AnimGraph::setClipNames(const QStringList& clips) {
    clips_ = clips;
    clips_.removeAll(QString());
    refreshAll(true);
}

void AnimGraph::commit(const ukiyo::AnimComponent& next, const QString& what, bool rebuildTables) {
    undo_.push_back(graph_);
    if (undo_.size() > 200) undo_.erase(undo_.begin());
    redo_.clear();
    applyGraph(next, what, rebuildTables);
}

void AnimGraph::applyGraph(const ukiyo::AnimComponent& next, const QString& what, bool rebuildTables) {
    graph_ = next;
    refreshAll(rebuildTables);
    status_->setText(what);
    if (instance_ >= 0) emit graphChanged(instance_, graph_);
}

bool AnimGraph::undo() {
    if (undo_.empty()) return false;
    redo_.push_back(graph_);
    const ukiyo::AnimComponent prev = undo_.back();
    undo_.pop_back();
    applyGraph(prev, "Undo", true);
    return true;
}

bool AnimGraph::redo() {
    if (redo_.empty()) return false;
    undo_.push_back(graph_);
    const ukiyo::AnimComponent next = redo_.back();
    redo_.pop_back();
    applyGraph(next, "Redo", true);
    return true;
}

QStringList AnimGraph::paramNames() const {
    QStringList names;
    for (const auto& p : graph_.params) names << QString::fromStdString(p.name);
    return names;
}

QString AnimGraph::uniqueStateName() const {
    for (int k = 1;; ++k) {
        const QString name = QStringLiteral("State %1").arg(k);
        if (stateIndex(graph_, name) < 0) return name;
    }
}

// ---------------------------------------------------------------- editing operations

void AnimGraph::addState(const QString& name, const QString& clip) {
    const QString n = name.isEmpty() ? uniqueStateName() : name.trimmed();
    if (n.isEmpty() || n == kAny || stateIndex(graph_, n) >= 0) {
        status_->setText("State names must be unique and not empty.");
        return;
    }
    ukiyo::AnimComponent next = graph_;
    next.custom = true;
    ukiyo::AnimStateDef s;
    s.name = n.toStdString();
    s.clip = clip.toStdString();
    // To the right of the rightmost state, so a new state never lands on another one.
    double right = -220.0;
    for (const auto& other : next.states) right = std::max(right, double(other.x));
    s.x = static_cast<float>(next.states.size() == 1 ? 0.0 : right + 220.0);
    s.y = static_cast<float>(next.states.size() == 1 ? 0.0 : next.states.front().y);
    next.states.push_back(s);
    if (next.defaultState.empty()) next.defaultState = s.name;
    selState_ = n;
    selTrans_ = -1;
    commit(next, "Added state");
}

void AnimGraph::addTransition(const QString& from, const QString& to) {
    if (from == to && from != kAny) {
        status_->setText("A state cannot transition to itself.");
        return;
    }
    if (to == kAny) {
        status_->setText("Pick a state as the target.");
        return;
    }
    ukiyo::AnimComponent next = graph_;
    next.custom = true;
    ukiyo::AnimTransitionDef t;
    t.from = from.toStdString();
    t.to = to.toStdString();
    t.fade = 0.25f;
    t.minDwell = 0.0f;
    next.transitions.push_back(t);
    selTrans_ = static_cast<int>(next.transitions.size()) - 1;
    selState_.clear();
    commit(next, "Added transition");
}

void AnimGraph::deleteState(const QString& name) {
    const int i = stateIndex(graph_, name);
    if (i < 0) return;
    ukiyo::AnimComponent next = graph_;
    next.states.erase(next.states.begin() + i);
    next.transitions.erase(std::remove_if(next.transitions.begin(), next.transitions.end(),
                                          [&](const ukiyo::AnimTransitionDef& t) {
                                              return QString::fromStdString(t.from) == name ||
                                                     QString::fromStdString(t.to) == name;
                                          }),
                           next.transitions.end());
    if (QString::fromStdString(next.defaultState) == name)
        next.defaultState = next.states.empty() ? std::string() : next.states.front().name;
    selState_.clear();
    selTrans_ = -1;
    commit(next, "Deleted state");
}

void AnimGraph::deleteTransition(int index) {
    if (index < 0 || index >= static_cast<int>(graph_.transitions.size())) return;
    ukiyo::AnimComponent next = graph_;
    next.transitions.erase(next.transitions.begin() + index);
    selTrans_ = -1;
    commit(next, "Deleted transition");
}

void AnimGraph::renameState(const QString& from, const QString& to) {
    const QString n = to.trimmed();
    if (from == n) return;
    if (n.isEmpty() || n == kAny || stateIndex(graph_, n) >= 0) {
        status_->setText("State names must be unique and not empty.");
        stateName_->setText(from);
        return;
    }
    ukiyo::AnimComponent next = graph_;
    const int i = stateIndex(graph_, from);
    if (i < 0) return;
    next.states[i].name = n.toStdString();
    for (auto& t : next.transitions) {
        if (t.from == from.toStdString()) t.from = n.toStdString();
        if (t.to == from.toStdString()) t.to = n.toStdString();
    }
    if (next.defaultState == from.toStdString()) next.defaultState = n.toStdString();
    selState_ = n;
    commit(next, "Renamed state");
}

void AnimGraph::setDefault(const QString& name) {
    ukiyo::AnimComponent next = graph_;
    next.defaultState = name.toStdString();
    commit(next, name.isEmpty() ? "Default is the first state" : "Set default state");
}

void AnimGraph::addParam(const QString& name, ParamType type, float value) {
    const QString n = name.trimmed();
    if (n.isEmpty() || paramNames().contains(n)) {
        status_->setText("Parameter names must be unique and not empty.");
        return;
    }
    ukiyo::AnimComponent next = graph_;
    ukiyo::AnimParamDef p;
    p.name = n.toStdString();
    p.type = type;
    p.value = value;
    next.params.push_back(p);
    commit(next, "Added parameter");
}

void AnimGraph::selectState(const QString& name) {
    selState_ = name;
    selTrans_ = -1;
    refreshAll(true);
}

void AnimGraph::selectTransition(int index) {
    selState_.clear();
    selTrans_ = index;
    refreshAll(true);
}

void AnimGraph::createFromLegacy() {
    if (instance_ < 0) return;
    ukiyo::AnimComponent next = graph_;
    next.custom = true;
    next.params.clear();
    ukiyo::AnimParamDef speed;
    speed.name = "speed";
    speed.type = ParamType::Float;
    speed.value = 0.0f;
    next.params.push_back(speed);
    next.states.clear();
    auto state = [&](const char* name, const std::string& clip, float x) {
        ukiyo::AnimStateDef s;
        s.name = name;
        s.clip = clip;
        s.x = x;
        s.y = 0.0f;
        next.states.push_back(s);
    };
    state("Idle", graph_.idleClip, -200.0f);
    state("Walk", graph_.walkClip, 0.0f);
    state("Run", graph_.runClip, 200.0f);
    next.defaultState = "Idle";
    next.transitions.clear();
    auto when = [](Cond::Op op, float v) {
        Cond c;
        c.param = "speed";
        c.op = op;
        c.value = v;
        return std::vector<Cond>{c};
    };
    auto add = [&](const char* from, const char* to, std::vector<Cond> cs) {
        ukiyo::AnimTransitionDef t;
        t.from = from;
        t.to = to;
        t.when = std::move(cs);
        t.fade = 0.2f;
        t.minDwell = 0.1f;
        next.transitions.push_back(t);
    };
    add("Idle", "Walk", when(Cond::Op::Greater, graph_.walkSpeed));
    add("Walk", "Run", when(Cond::Op::Greater, graph_.runSpeed));
    add("Run", "Walk", when(Cond::Op::Less, graph_.runSpeed));
    add("Walk", "Idle", when(Cond::Op::Less, graph_.walkSpeed));
    selState_.clear();
    selTrans_ = -1;
    commit(next, "Created the graph from the three-clip machine");
}

void AnimGraph::onDeleteKey() {
    if (!selState_.isEmpty()) deleteState(selState_);
    else if (selTrans_ >= 0) deleteTransition(selTrans_);
}

void AnimGraph::onNodePicked(const QString& name) {
    if (!graph_.custom) return;
    if (connectFrom_.isEmpty()) {
        connectFrom_ = name;
        status_->setText(QString("From %1: click the target state.").arg(name == kAny ? QStringLiteral("any state") : name));
        return;
    }
    const QString from = connectFrom_;
    connectFrom_.clear();
    addTransition(from, name);
}

void AnimGraph::onLayoutMoved() {
    if (instance_ < 0 || !graph_.custom) return;
    ukiyo::AnimComponent next = graph_;
    bool changed = false;
    for (QGraphicsItem* it : scene_->items()) {
        auto* n = dynamic_cast<StateNode*>(it);
        if (!n || n->any_) continue;
        const int i = stateIndex(next, n->name_);
        if (i < 0) continue;
        if (std::abs(next.states[i].x - n->pos().x()) > 0.5 || std::abs(next.states[i].y - n->pos().y()) > 0.5) {
            next.states[i].x = static_cast<float>(n->pos().x());
            next.states[i].y = static_cast<float>(n->pos().y());
            changed = true;
        }
    }
    if (!changed) return;
    // Layout is not an edit: it does not add an undo step.
    graph_ = next;
    emit graphChanged(instance_, graph_);
}

void AnimGraph::onRenameEdited() {
    if (refreshing_ || selState_.isEmpty()) return;
    renameState(selState_, stateName_->text());
}

void AnimGraph::onSceneSelection() {
    if (refreshing_) return;
    selState_.clear();
    selTrans_ = -1;
    for (QGraphicsItem* it : scene_->selectedItems()) {
        if (auto* n = dynamic_cast<StateNode*>(it)) {
            if (!n->any_) selState_ = n->name_;
        } else if (auto* t = dynamic_cast<TransitionItem*>(it)) {
            selTrans_ = t->index();
        }
    }
    refreshSide(true);
    deleteBtn_->setEnabled(!selState_.isEmpty() || selTrans_ >= 0);
    defaultBtn_->setEnabled(!selState_.isEmpty());
}

void AnimGraph::writeBlendKind() {
    const int i = stateIndex(graph_, selState_);
    if (i < 0) return;
    const auto kind = static_cast<ukiyo::BlendKind>(blendKind_->currentData().toInt());
    ukiyo::AnimComponent next = graph_;
    next.states[i].blend = kind;
    if (kind == ukiyo::BlendKind::Linear1D) next.states[i].paramY.clear();
    commit(next, "Changed the motion type", true);
}

void AnimGraph::writeBlendParams() {
    const int i = stateIndex(graph_, selState_);
    if (i < 0) return;
    ukiyo::AnimComponent next = graph_;
    next.states[i].paramX = paramX_->currentData().toString().toStdString();
    next.states[i].paramY = next.states[i].blend == ukiyo::BlendKind::Linear1D
                                ? std::string()
                                : paramY_->currentData().toString().toStdString();
    commit(next, "Changed a blend parameter", false);
}

void AnimGraph::writeMotions() {
    const int i = stateIndex(graph_, selState_);
    if (i < 0) return;
    ukiyo::AnimComponent next = graph_;
    std::vector<ukiyo::BlendMotion> list;
    for (int r = 0; r < motions_->rowCount(); ++r) {
        auto* clip = qobject_cast<QComboBox*>(motions_->cellWidget(r, 0));
        auto* xs = qobject_cast<QDoubleSpinBox*>(motions_->cellWidget(r, 1));
        auto* ys = qobject_cast<QDoubleSpinBox*>(motions_->cellWidget(r, 2));
        auto* ts = qobject_cast<QDoubleSpinBox*>(motions_->cellWidget(r, 3));
        auto* mir = qobject_cast<QCheckBox*>(motions_->cellWidget(r, 4));
        auto* par = qobject_cast<QComboBox*>(motions_->cellWidget(r, 5));
        if (!clip || !xs || !ys || !ts || !mir || !par) continue;
        ukiyo::BlendMotion m;
        m.clip = clip->currentData().toString().toStdString();
        m.x = static_cast<float>(xs->value());
        m.y = static_cast<float>(ys->value());
        m.timeScale = static_cast<float>(ts->value());
        m.mirror = mir->isChecked();
        m.param = par->currentData().toString().toStdString();
        list.push_back(m);
    }
    next.states[i].motions = std::move(list);
    commit(next, "Edited a motion", false);
}

void AnimGraph::refreshBlendControls(int i, bool rebuildTables) {
    const ukiyo::AnimStateDef& s = graph_.states[i];
    const bool blended = s.blend != ukiyo::BlendKind::None;
    const bool direct = s.blend == ukiyo::BlendKind::Direct;
    const bool twoD = s.blend == ukiyo::BlendKind::SimpleDirectional2D ||
                      s.blend == ukiyo::BlendKind::FreeformCartesian2D ||
                      s.blend == ukiyo::BlendKind::FreeformDirectional2D;
    {
        QSignalBlocker b(blendKind_);
        const int at = blendKind_->findData(static_cast<int>(s.blend));
        blendKind_->setCurrentIndex(at < 0 ? 0 : at);
    }
    // Only float parameters can drive a blend, so the lists hold those alone.
    auto fill = [this](QComboBox* box, const std::string& current) {
        QSignalBlocker b(box);
        box->clear();
        box->addItem("(none)", QString());
        for (const auto& p : graph_.params)
            if (p.type == ukiyo::AnimParamDef::Type::Float)
                box->addItem(QString::fromStdString(p.name), QString::fromStdString(p.name));
        const QString cur = QString::fromStdString(current);
        if (cur.isEmpty()) return;
        const int at = box->findData(cur);
        if (at >= 0) box->setCurrentIndex(at);
        else box->addItem(cur + " (missing)", cur), box->setCurrentIndex(box->count() - 1);
    };
    fill(paramX_, s.paramX);
    fill(paramY_, s.paramY);
    stateClip_->setEnabled(!blended);
    paramX_->setEnabled(blended && !direct);
    paramY_->setEnabled(twoD);
    motions_->setEnabled(blended);
    addMotionBtn_->setEnabled(blended);
    delMotionBtn_->setEnabled(blended);
    autoMotionBtn_->setEnabled(s.blend == ukiyo::BlendKind::Linear1D && s.motions.size() >= 2);
    if (rebuildTables) {
        motions_->setRowCount(0);
        for (int r = 0; r < static_cast<int>(s.motions.size()); ++r) {
            const ukiyo::BlendMotion& m = s.motions[r];
            motions_->insertRow(r);
            auto* clip = new QComboBox;
            for (const QString& c : clips_) clip->addItem(c, c);
            const QString cur = QString::fromStdString(m.clip);
            const int at = clip->findData(cur);
            if (at >= 0) clip->setCurrentIndex(at);
            else if (!cur.isEmpty()) clip->addItem(cur + " (missing)", cur), clip->setCurrentIndex(clip->count() - 1);
            auto* xs = new QDoubleSpinBox;
            xs->setRange(-10000.0, 10000.0);
            xs->setDecimals(3);
            xs->setValue(m.x);
            xs->setEnabled(!direct);
            auto* ys = new QDoubleSpinBox;
            ys->setRange(-10000.0, 10000.0);
            ys->setDecimals(3);
            ys->setValue(m.y);
            ys->setEnabled(twoD);
            auto* ts = new QDoubleSpinBox;
            ts->setRange(0.05, 10.0);
            ts->setDecimals(2);
            ts->setSingleStep(0.05);
            ts->setValue(m.timeScale);
            auto* mir = new QCheckBox;
            mir->setChecked(m.mirror);
            mir->setToolTip("Play this clip reflected left to right");
            auto* par = new QComboBox;
            par->addItem("(none)", QString());
            for (const auto& p : graph_.params)
                if (p.type == ukiyo::AnimParamDef::Type::Float)
                    par->addItem(QString::fromStdString(p.name), QString::fromStdString(p.name));
            {
                const QString mp = QString::fromStdString(m.param);
                const int pa = par->findData(mp);
                if (pa >= 0) par->setCurrentIndex(pa);
                else if (!mp.isEmpty()) par->addItem(mp + " (missing)", mp), par->setCurrentIndex(par->count() - 1);
            }
            par->setEnabled(direct);
            motions_->setCellWidget(r, 0, clip);
            motions_->setCellWidget(r, 1, xs);
            motions_->setCellWidget(r, 2, ys);
            motions_->setCellWidget(r, 3, ts);
            motions_->setCellWidget(r, 4, mir);
            motions_->setCellWidget(r, 5, par);
            connect(ts, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) {
                if (!refreshing_) writeMotions();
            });
            connect(mir, &QCheckBox::toggled, this, [this](bool) {
                if (!refreshing_) writeMotions();
            });
            connect(par, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
                if (!refreshing_) writeMotions();
            });
            connect(clip, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
                if (!refreshing_) writeMotions();
            });
            connect(xs, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) {
                if (!refreshing_) writeMotions();
            });
            connect(ys, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) {
                if (!refreshing_) writeMotions();
            });
        }
    }
    switch (s.blend) {
    case ukiyo::BlendKind::None:
        blendHint_->setText("Single clip: the state plays one clip. Choose a motion type to mix several clips by parameters.");
        break;
    case ukiyo::BlendKind::Linear1D:
        blendHint_->setText("1D blend: the motions sit on the X axis. The weight moves between the two nearest ones.");
        break;
    case ukiyo::BlendKind::SimpleDirectional2D:
        blendHint_->setText("2D simple directional: each motion is a direction. The motion at (0, 0) is the idle pose.");
        break;
    case ukiyo::BlendKind::FreeformCartesian2D:
        blendHint_->setText("2D freeform: each motion is a point in the XY plane. Weights fall off with distance.");
        break;
    case ukiyo::BlendKind::FreeformDirectional2D:
        blendHint_->setText("2D freeform directional: each motion is a direction and a length. The motion at (0, 0) is the idle pose. Several motions may share a direction.");
        break;
    case ukiyo::BlendKind::Direct:
        blendHint_->setText("Direct: each motion has its own float parameter. Its value sets the motion's weight, and the weights are normalised.");
        break;
    }
}

void AnimGraph::setStateBlend(const QString& state, ukiyo::BlendKind kind, const QString& paramX, const QString& paramY) {
    const int i = stateIndex(graph_, state);
    if (i < 0) return;
    ukiyo::AnimComponent next = graph_;
    next.states[i].blend = kind;
    next.states[i].paramX = paramX.toStdString();
    next.states[i].paramY = kind == ukiyo::BlendKind::Linear1D ? std::string() : paramY.toStdString();
    commit(next, "Set the motion type", true);
}

void AnimGraph::addMotion(const QString& state, const QString& clip, float x, float y) {
    const int i = stateIndex(graph_, state);
    if (i < 0) return;
    ukiyo::AnimComponent next = graph_;
    ukiyo::BlendMotion m;
    m.clip = clip.toStdString();
    m.x = x;
    m.y = y;
    next.states[i].motions.push_back(m);
    commit(next, "Added a motion", true);
}

int AnimGraph::motionCount(const QString& state) const {
    const int i = stateIndex(graph_, state);
    return i < 0 ? -1 : static_cast<int>(graph_.states[i].motions.size());
}

void AnimGraph::writeConditions() {
    if (refreshing_ || selTrans_ < 0 || selTrans_ >= static_cast<int>(graph_.transitions.size())) return;
    ukiyo::AnimComponent next = graph_;
    auto& when = next.transitions[selTrans_].when;
    when.clear();
    for (int r = 0; r < conds_->rowCount(); ++r) {
        auto* pc = qobject_cast<QComboBox*>(conds_->cellWidget(r, 0));
        auto* oc = qobject_cast<QComboBox*>(conds_->cellWidget(r, 1));
        auto* vs = qobject_cast<QDoubleSpinBox*>(conds_->cellWidget(r, 2));
        if (!pc || !oc || !vs) continue;
        Cond c;
        c.param = pc->currentText().toStdString();
        c.op = static_cast<Cond::Op>(oc->currentData().toInt());
        c.value = static_cast<float>(vs->value());
        when.push_back(c);
    }
    commit(next, "Changed conditions", false);
}

void AnimGraph::writeParams() {
    if (refreshing_) return;
    ukiyo::AnimComponent next = graph_;
    next.params.clear();
    for (int r = 0; r < params_->rowCount(); ++r) {
        auto* name = qobject_cast<QLineEdit*>(params_->cellWidget(r, 0));
        auto* type = qobject_cast<QComboBox*>(params_->cellWidget(r, 1));
        auto* value = qobject_cast<QDoubleSpinBox*>(params_->cellWidget(r, 2));
        if (!name || !type || !value) continue;
        ukiyo::AnimParamDef p;
        p.name = name->text().trimmed().toStdString();
        p.type = static_cast<ParamType>(type->currentIndex());
        p.value = static_cast<float>(value->value());
        next.params.push_back(p);
    }
    commit(next, "Changed parameters", false);
}

// ---------------------------------------------------------------- refresh

void AnimGraph::refreshAll(bool rebuildTables) {
    refreshing_ = true;
    {
        QSignalBlocker b(customBox_);
        customBox_->setChecked(graph_.custom);
    }
    const bool attached = instance_ >= 0;
    const bool editable = attached && graph_.custom;
    connectBtn_->setEnabled(editable);
    addStateBtn_->setEnabled(editable);
    legacyBtn_->setEnabled(attached && graph_.states.empty());
    customBox_->setEnabled(attached);
    if (!attached) hint_->setText("Select an object with locomotion to edit its state graph.");
    else if (!graph_.custom) hint_->setText("This object uses the three-clip Idle, Walk and Run machine. Tick Custom graph to edit states.");
    else hint_->setText("Drag states to arrange them. Connect: click a source, then a target. Delete removes the selection. Ctrl+Z undoes.");
    if (!editable) connectBtn_->setChecked(false);

    rebuildScene();
    refreshSide(rebuildTables);
    if (rebuildTables) refreshParams();
    refreshProblems();
    rebuildTestControls();
    rebuildTestMachine();
    refreshing_ = false;
}

void AnimGraph::rebuildScene() {
    scene_->clear();
    std::map<QString, StateNode*> nodes;
    if (graph_.custom) {
        auto* any = new StateNode(kAny, QString(), false, true);
        any->setPos(-270, -150);
        scene_->addItem(any);
        nodes[kAny] = any;
    }
    for (const auto& s : graph_.states) {
        const QString name = QString::fromStdString(s.name);
        const bool isDefault = graph_.defaultState.empty() ? (&s == &graph_.states.front())
                                                           : (graph_.defaultState == s.name);
        const QString label = s.blend == ukiyo::BlendKind::None
                                  ? QString::fromStdString(s.clip)
                                  : QStringLiteral("blend: %1 motions").arg(static_cast<int>(s.motions.size()));
        auto* n = new StateNode(name, label, isDefault, false);
        n->setPos(s.x, s.y);
        scene_->addItem(n);
        nodes[name] = n;
        n->setSelected(name == selState_);
    }
    std::map<std::pair<QString, QString>, int> total, seen;
    for (const auto& t : graph_.transitions) total[{QString::fromStdString(t.from), QString::fromStdString(t.to)}]++;
    for (int i = 0; i < static_cast<int>(graph_.transitions.size()); ++i) {
        const auto& t = graph_.transitions[i];
        const QString from = QString::fromStdString(t.from);
        const QString to = QString::fromStdString(t.to);
        if (!nodes.count(from) || !nodes.count(to)) continue; // reported in the checks list
        const std::pair<QString, QString> key{from, to};
        const int k = seen[key]++;
        const double lane = k - (total[key] - 1) / 2.0;
        auto* item = new TransitionItem(from, to, i, lane, transLabel(t));
        scene_->addItem(item);
        item->updatePath();
        item->setSelected(i == selTrans_);
    }
    if (!scene_->items().isEmpty()) {
        view_->setSceneRect(scene_->itemsBoundingRect().adjusted(-220, -180, 220, 180));
        view_->fitInView(scene_->itemsBoundingRect().adjusted(-60, -60, 60, 60), Qt::KeepAspectRatio);
    }
}

void AnimGraph::refreshSide(bool rebuildTables) {
    const bool hasState = !selState_.isEmpty() && stateIndex(graph_, selState_) >= 0;
    const bool hasTrans = !hasState && selTrans_ >= 0 && selTrans_ < static_cast<int>(graph_.transitions.size());
    const bool wasRefreshing = refreshing_;
    refreshing_ = true;
    if (hasState) {
        side_->setCurrentIndex(1);
        const int i = stateIndex(graph_, selState_);
        stateName_->setText(selState_);
        {
            QSignalBlocker b(stateClip_);
            stateClip_->clear();
            stateClip_->addItem("(idle loop)", QString());
            for (const QString& c : clips_) stateClip_->addItem(c, c);
            const QString clip = QString::fromStdString(graph_.states[i].clip);
            const int at = stateClip_->findData(clip);
            if (at >= 0) stateClip_->setCurrentIndex(at);
            else if (!clip.isEmpty()) stateClip_->addItem(clip + " (missing)", clip), stateClip_->setCurrentIndex(stateClip_->count() - 1);
        }
        const bool isDefault = graph_.defaultState.empty() ? i == 0 : graph_.defaultState == graph_.states[i].name;
        {
            QSignalBlocker b(stateDefault_);
            stateDefault_->setChecked(isDefault);
        }
        refreshBlendControls(i, rebuildTables);
    } else if (hasTrans) {
        side_->setCurrentIndex(2);
        const auto& t = graph_.transitions[selTrans_];
        transRoute_->setText(QString("%1  →  %2").arg(t.from == kAny.toStdString() ? QStringLiteral("any state")
                                                                                  : QString::fromStdString(t.from))
                                 .arg(QString::fromStdString(t.to)));
        {
            QSignalBlocker b1(fade_), b2(dwell_), b3(exitCheck_), b4(exitTime_), b5(fixed_), b6(offset_), b7(selfCheck_);
            fade_->setValue(t.fade);
            dwell_->setValue(t.minDwell);
            exitCheck_->setChecked(t.opts.hasExitTime);
            exitTime_->setValue(t.opts.exitTime);
            fixed_->setChecked(t.opts.fixedDuration);
            offset_->setValue(t.opts.offset);
            selfCheck_->setChecked(t.opts.toSelf);
        }
        if (rebuildTables) {
            conds_->setRowCount(0);
            const QStringList names = paramNames();
            for (int r = 0; r < static_cast<int>(t.when.size()); ++r) {
                const Cond& c = t.when[r];
                conds_->insertRow(r);
                auto* pc = new QComboBox;
                pc->addItems(names);
                const QString p = QString::fromStdString(c.param);
                if (!names.contains(p)) pc->addItem(p);
                pc->setCurrentText(p);
                auto* oc = new QComboBox;
                for (Cond::Op op : kOps) oc->addItem(opLabel(op), static_cast<int>(op));
                oc->setCurrentIndex(static_cast<int>(std::find(std::begin(kOps), std::end(kOps), c.op) - std::begin(kOps)));
                auto* vs = new QDoubleSpinBox;
                vs->setRange(-10000.0, 10000.0);
                vs->setDecimals(3);
                vs->setValue(c.value);
                vs->setEnabled(opNeedsValue(c.op));
                conds_->setCellWidget(r, 0, pc);
                conds_->setCellWidget(r, 1, oc);
                conds_->setCellWidget(r, 2, vs);
                connect(pc, &QComboBox::currentTextChanged, this, [this](const QString&) { writeConditions(); });
                connect(oc, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, oc, vs](int) {
                    vs->setEnabled(opNeedsValue(static_cast<Cond::Op>(oc->currentData().toInt())));
                    writeConditions();
                });
                connect(vs, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) { writeConditions(); });
            }
        }
    } else {
        side_->setCurrentIndex(0);
    }
    refreshing_ = wasRefreshing;
    deleteTransBtn_->setEnabled(hasTrans);
    addCondBtn_->setEnabled(hasTrans);
    delCondBtn_->setEnabled(hasTrans);
    stateName_->setEnabled(hasState);
    stateClip_->setEnabled(hasState);
    stateDefault_->setEnabled(hasState);
    fade_->setEnabled(hasTrans);
    dwell_->setEnabled(hasTrans);
    exitCheck_->setEnabled(hasTrans);
    exitTime_->setEnabled(hasTrans && exitCheck_->isChecked());
    fixed_->setEnabled(hasTrans);
    offset_->setEnabled(hasTrans);
    selfCheck_->setEnabled(hasTrans && graph_.transitions[selTrans_].from == kAny.toStdString());
}

void AnimGraph::refreshParams() {
    const bool wasRefreshing = refreshing_;
    refreshing_ = true;
    params_->setRowCount(0);
    for (int r = 0; r < static_cast<int>(graph_.params.size()); ++r) {
        const auto& p = graph_.params[r];
        params_->insertRow(r);
        auto* name = new QLineEdit(QString::fromStdString(p.name));
        auto* type = new QComboBox;
        for (ParamType t : {ParamType::Float, ParamType::Bool, ParamType::Trigger}) type->addItem(typeLabel(t));
        type->setCurrentIndex(static_cast<int>(p.type));
        auto* value = new QDoubleSpinBox;
        value->setRange(-10000.0, 10000.0);
        value->setDecimals(3);
        value->setValue(p.value);
        value->setEnabled(p.type != ParamType::Trigger);
        params_->setCellWidget(r, 0, name);
        params_->setCellWidget(r, 1, type);
        params_->setCellWidget(r, 2, value);
        connect(name, &QLineEdit::editingFinished, this, [this] { writeParams(); });
        connect(type, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, type, value](int) {
            value->setEnabled(type->currentIndex() != static_cast<int>(ParamType::Trigger));
            writeParams();
        });
        connect(value, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) { writeParams(); });
    }
    refreshing_ = wasRefreshing;
}

QStringList AnimGraph::problems() const {
    return problemList();
}

QStringList AnimGraph::problemList() const {
    if (!graph_.custom) return {};
    return animfile::validateGraph(graph_, clips_);
}

void AnimGraph::refreshProblems() {
    problems_->clear();
    const QStringList list = problemList();
    if (list.isEmpty()) {
        problems_->addItem(graph_.custom ? QStringLiteral("No problems. The graph can run.")
                                         : QStringLiteral("Not using the custom graph."));
        return;
    }
    for (const QString& p : list) problems_->addItem(p);
}

void AnimGraph::rebuildTestControls() {
    QStringList sig;
    for (const auto& p : graph_.params) sig << QString::fromStdString(p.name) + ":" + typeLabel(p.type);
    if (sig == testParamNames_ && testControls_) return;
    testParamNames_ = sig;
    delete testControls_;
    testControls_ = new QWidget;
    auto* form = new QFormLayout(testControls_);
    form->setContentsMargins(0, 0, 0, 0);
    for (const auto& p : graph_.params) {
        const QString name = QString::fromStdString(p.name);
        QWidget* w = nullptr;
        if (p.type == ParamType::Float) {
            auto* s = new QDoubleSpinBox;
            s->setRange(-100.0, 100.0);
            s->setDecimals(2);
            s->setSingleStep(0.1);
            s->setValue(p.value);
            s->setObjectName(name);
            connect(s, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, name](double v) {
                if (test_) test_->setFloat(name.toStdString(), static_cast<float>(v));
                testAdvance(0.05f);
            });
            w = s;
        } else if (p.type == ParamType::Bool) {
            auto* c = new QCheckBox;
            c->setChecked(p.value != 0.0f);
            c->setObjectName(name);
            connect(c, &QCheckBox::toggled, this, [this, name](bool on) {
                if (test_) test_->setBool(name.toStdString(), on);
                testAdvance(0.05f);
            });
            w = c;
        } else {
            auto* b = new QPushButton("Fire");
            b->setObjectName(name);
            connect(b, &QPushButton::clicked, this, [this, name] {
                if (test_) test_->setTrigger(name.toStdString());
                testAdvance(0.05f);
            });
            w = b;
        }
        form->addRow(name, w);
    }
    static_cast<QVBoxLayout*>(testBox_->layout())->insertWidget(0, testControls_);
}

bool testRunnable(const ukiyo::AnimComponent& g) {
    if (!g.custom || g.states.empty()) return false;
    auto known = [&](const std::string& s) {
        if (s == "*") return true;
        return std::any_of(g.states.begin(), g.states.end(), [&](const ukiyo::AnimStateDef& d) { return d.name == s; });
    };
    if (!g.defaultState.empty() && !known(g.defaultState)) return false;
    for (const auto& t : g.transitions)
        if (!known(t.from) || !known(t.to)) return false;
    return true;
}

void AnimGraph::rebuildTestMachine() {
    testLog_->clear();
    if (!testRunnable(graph_)) {
        test_.reset();
        testStateLabel_->setText(graph_.custom ? "Add states and fix the checks to test the graph."
                                               : "Not using the custom graph.");
        return;
    }
    test_ = animfile::buildStateMachine(graph_, durations_);
    for (const auto& p : graph_.params) {
        const QString name = QString::fromStdString(p.name);
        QWidget* w = testControls_ ? testControls_->findChild<QWidget*>(name) : nullptr;
        if (auto* s = qobject_cast<QDoubleSpinBox*>(w)) test_->setFloat(p.name, static_cast<float>(s->value()));
        else if (auto* c = qobject_cast<QCheckBox*>(w)) test_->setBool(p.name, c->isChecked());
    }
    testLastBlend_.clear();
    test_->update(0.0f, [](const std::string&, float, float) {},
                  [](const std::vector<std::string>&, const std::vector<float>&, const std::vector<ukiyo::MotionOptions>&, float) {});
    testStateLabel_->setText(QString("Current state: %1").arg(QString::fromStdString(test_->current())));
}

void AnimGraph::testAdvance(float seconds) {
    if (!test_) return;
    const int steps = std::max(1, static_cast<int>(std::lround(seconds / 0.05f)));
    auto onBlend = [this](const std::vector<std::string>& clips, const std::vector<float>& weights,
                          const std::vector<ukiyo::MotionOptions>&, float fade) {
        QStringList parts;
        for (size_t k = 0; k < clips.size(); ++k)
            parts << QStringLiteral("%1 %2").arg(QString::fromStdString(clips[k]))
                                             .arg(k < weights.size() ? weights[k] : 0.0f, 0, 'f', 2);
        const QString text = QStringLiteral("-> blend: %1").arg(parts.join(", "));
        // Entering a blend always logs. A weight change logs only when the mix moved, so the log stays readable.
        if (fade >= 0.0f) testLog_->addItem(QStringLiteral("%1   (%2 s crossfade)").arg(text).arg(fade, 0, 'f', 2));
        else if (text != testLastBlend_) testLog_->addItem(text);
        testLastBlend_ = text;
    };
    for (int i = 0; i < steps; ++i) {
        test_->update(0.05f, [this](const std::string& clip, float fade, float startFraction) {
            testLog_->addItem(QString("-> %1   (%2 s crossfade, start at %3)")
                                  .arg(clip.empty() ? QStringLiteral("idle loop") : QString::fromStdString(clip))
                                  .arg(fade, 0, 'f', 2)
                                  .arg(startFraction, 0, 'f', 2));
        }, onBlend);
    }
    testStateLabel_->setText(QString("Current state: %1").arg(QString::fromStdString(test_->current())));
}

QString AnimGraph::testState() const {
    return test_ ? QString::fromStdString(test_->current()) : QString();
}

void AnimGraph::testSetFloat(const QString& name, float v) {
    if (!test_) return;
    test_->setFloat(name.toStdString(), v);
    testAdvance(0.05f);
}
