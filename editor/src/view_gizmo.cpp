#include "view_gizmo.h"

#include <QFont>
#include <QString>
#include <QPainter>
#include <QPen>

#include <algorithm>
#include <cmath>
#include <vector>

namespace editor::viewgizmo {

namespace {
constexpr double kMargin = 12.0;       // from the view's edges
constexpr double kSize = 96.0;         // the widget's box, in pixels
constexpr double kBallRadius = 13.0;   // the ball at the end of each axis
constexpr double kAxisLength = 0.78;   // where the balls sit, in box radii
constexpr double kHitRadius = 15.0;

// The six directions, in the order Blender draws them: X, -X, Y, -Y, Z, -Z.
const glm::vec3 kDirs[kHandleCount] = {
    {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
};
} // namespace

glm::vec3 handleDirection(int index) {
    if (index < 0 || index >= kHandleCount) return glm::vec3(0.0f);
    return kDirs[index];
}

const char* handleLabel(int index) {
    switch (index) {
    case 0: return "X";
    case 2: return "Y";
    case 4: return "Z";
    default: return "";
    }
}

QColor handleColour(int index) {
    switch (index) {
    case 0: case 1: return QColor(229, 72, 77);
    case 2: case 3: return QColor(70, 167, 88);
    case 4: case 5: return QColor(62, 99, 221);
    default: return QColor(200, 200, 200);
    }
}

QRectF bounds(int viewWidth, int viewHeight) {
    const double x = std::max(double(viewWidth) - kSize - kMargin, 0.0);
    return QRectF(x, kMargin, kSize, kSize);
}

QPointF handlePoint(const ukiyo::OrbitCamera& cam, const QRectF& box, int index, float* depth) {
    // The camera's rotation only: the widget is a compass, so where the eye is does not matter, only which way it looks.
    const glm::mat3 rot(cam.view());
    const glm::vec3 v = rot * handleDirection(index);   // view space: +x right, +y up, -z into the screen
    const double r = 0.5 * std::min(box.width(), box.height()) * kAxisLength;
    const QPointF centre = box.center();
    if (depth) *depth = float(-v.z);                     // nearer the viewer is larger
    return QPointF(centre.x() + double(v.x) * r, centre.y() - double(v.y) * r);
}

int hitTest(const ukiyo::OrbitCamera& cam, const QRectF& box, QPointF mouse) {
    int best = -1;
    float bestDepth = -1e9f;
    double bestDist = kHitRadius;
    for (int i = 0; i < kHandleCount; ++i) {
        float depth = 0.0f;
        const QPointF s = handlePoint(cam, box, i, &depth);
        const double d = std::hypot(mouse.x() - s.x(), mouse.y() - s.y());
        if (d > kHitRadius) continue;
        // Nearest the pointer wins; at the same distance, the handle in front wins.
        if (d < bestDist - 0.5 || (d <= bestDist + 0.5 && depth > bestDepth)) {
            bestDist = std::min(bestDist, d);
            bestDepth = depth;
            best = i;
        }
    }
    return best;
}

void viewFor(int index, float* yaw, float* pitch) {
    // The camera's eye sits at target + d*(sin(yaw)cos(pitch), sin(pitch), cos(yaw)cos(pitch)), so looking along +X means
    // standing on +X (yaw 90 degrees), looking along +Z means yaw 0, and the poles are just short of vertical because a
    // look-at with the world up as its up is undefined there.
    constexpr float kPole = 1.55334f;   // 89 degrees
    switch (index) {
    case 0: *yaw = 1.5707963f; *pitch = 0.0f; break;      // look along -X: from +X
    case 1: *yaw = -1.5707963f; *pitch = 0.0f; break;     // from -X
    case 2: *yaw = 0.0f; *pitch = kPole; break;           // from above
    case 3: *yaw = 0.0f; *pitch = -kPole; break;          // from below
    case 4: *yaw = 0.0f; *pitch = 0.0f; break;            // from +Z
    case 5: *yaw = 3.1415927f; *pitch = 0.0f; break;      // from -Z
    default: break;
    }
}

void paint(QPainter& p, const ukiyo::OrbitCamera& cam, const QRectF& box, int hover) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);

    // Back to front, so a ball in front covers the line of the one behind it.
    struct Handle {
        int index;
        QPointF at;
        float depth;
    };
    std::vector<Handle> handles;
    handles.reserve(kHandleCount);
    for (int i = 0; i < kHandleCount; ++i) {
        float depth = 0.0f;
        const QPointF at = handlePoint(cam, box, i, &depth);
        handles.push_back({i, at, depth});
    }
    std::sort(handles.begin(), handles.end(), [](const Handle& a, const Handle& b) { return a.depth < b.depth; });

    const QPointF centre = box.center();
    const QColor background(24, 24, 28);   // the view's own dark grey; a far handle is mixed toward this
    for (const Handle& h : handles) {
        // Depth 0 is the rim of the sphere, 1 is nearest: a far handle fades toward the background rather than being
        // hidden, which is what tells the eye it is behind.
        const double fade = std::clamp((double(h.depth) + 1.0) * 0.5, 0.0, 1.0);
        QColor colour = handleColour(h.index);
        const double mix = 0.25 + 0.75 * fade;
        colour = QColor(int(background.red() + (colour.red() - background.red()) * mix),
                        int(background.green() + (colour.green() - background.green()) * mix),
                        int(background.blue() + (colour.blue() - background.blue()) * mix));
        if (h.index == hover) colour = QColor(255, 224, 102);

        // A line from the centre for the positive axes only: the negative ones are the balls on the far side of the
        // sphere, which is how Blender draws it.
        if (h.index % 2 == 0) {
            QColor line = colour;
            line.setAlpha(200);
            p.setPen(QPen(line, 2.0));
            p.drawLine(centre, h.at);
        }

        p.setPen(QPen(colour.darker(140), 1.2));
        p.setBrush(colour);
        p.drawEllipse(h.at, kBallRadius, kBallRadius);

        const char* label = handleLabel(h.index);
        if (label[0] != '\0') {
            QFont f = p.font();
            f.setBold(true);
            f.setPixelSize(12);
            p.setFont(f);
            p.setPen(QColor(16, 16, 18));
            p.drawText(QRectF(h.at.x() - kBallRadius, h.at.y() - kBallRadius, kBallRadius * 2.0, kBallRadius * 2.0),
                       Qt::AlignCenter, QString::fromLatin1(label));
        }
    }
    p.restore();
}

} // namespace editor::viewgizmo
