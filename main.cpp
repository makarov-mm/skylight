// Skylight — physically based sky: Rayleigh + Mie single scattering
// Raymarched atmosphere shell around a spherical planet. One altitude slider
// takes you from a sunset on the ground to the blue rim seen from orbit —
// same equations, no special cases. CPU raymarcher, multithreaded, rendered
// at reduced resolution and upscaled; re-rendered only when parameters change.
// C++17 / Qt6 Widgets, no other dependencies.

#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QTimer>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QSlider>
#include <QLabel>
#include <QGroupBox>
#include <QStyleFactory>

#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(float a, float b, float c) : x(a), y(b), z(c) {}
    Vec3 operator+(const Vec3 &o) const { return { x + o.x, y + o.y, z + o.z }; }
    Vec3 operator-(const Vec3 &o) const { return { x - o.x, y - o.y, z - o.z }; }
    Vec3 operator*(float s) const { return { x * s, y * s, z * s }; }
    Vec3 operator*(const Vec3 &o) const { return { x * o.x, y * o.y, z * o.z }; }
    float dot(const Vec3 &o) const { return x * o.x + y * o.y + z * o.z; }
    float len() const { return std::sqrt(dot(*this)); }
    Vec3 norm() const { float l = len(); return { x / l, y / l, z / l }; }
};

// Physical constants (meters)
static constexpr float Rg = 6360e3f;              // ground radius
static constexpr float Ra = 6420e3f;              // atmosphere top
static constexpr float HR = 7994.f;               // Rayleigh scale height
static constexpr float HM = 1200.f;               // Mie scale height
static const Vec3 BETA_R { 5.8e-6f, 13.5e-6f, 33.1e-6f }; // Rayleigh sea level
static constexpr float BETA_M = 21e-6f;           // Mie sea level

struct SimParams {
    float altitudeKm  = 1.0f;    // camera height above ground
    float sunElevDeg  = 12.f;
    float pitchDeg    = 8.f;     // camera look elevation
    float mieG        = 0.76f;
    float turbidity   = 1.0f;    // multiplier on Mie
    float exposure    = 1.2f;
    int   quality     = 1;       // 0 low / 1 medium / 2 high
    int   resScale    = 3;       // render at 1/resScale
};

// ray-sphere: returns t0, t1 (may be negative); false if no hit
static bool raySphere(const Vec3 &o, const Vec3 &d, float R,
                      float &t0, float &t1)
{
    const float b = o.dot(d);
    const float c = o.dot(o) - R * R;
    const float disc = b * b - c;
    if (disc < 0.f) return false;
    const float s = std::sqrt(disc);
    t0 = -b - s;
    t1 = -b + s;
    return true;
}

class SkyCanvas : public QWidget
{
    Q_OBJECT
public:
    explicit SkyCanvas(QWidget *parent = nullptr)
    {
        setMinimumSize(640, 420);
        if (const char *e = getenv("ATMO_SUN"))
            params.sunElevDeg = float(atof(e));
        if (const char *e = getenv("ATMO_ALT"))
            params.altitudeKm = float(atof(e));
        if (const char *e = getenv("ATMO_PITCH"))
            params.pitchDeg = float(atof(e));
        connect(&m_timer, &QTimer::timeout, this, &SkyCanvas::frame);
        m_timer.start(33);
    }

    SimParams params;
    void invalidate() { m_dirty = true; }
    float lastRenderMs() const { return m_renderMs; }

signals:
    void statsChanged();

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(rect(), m_lowres);
    }

    void resizeEvent(QResizeEvent *) override { m_dirty = true; }

    void mousePressEvent(QMouseEvent *e) override { m_last = e->pos(); }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (e->buttons() & Qt::LeftButton) {
            params.pitchDeg += (e->pos().y() - m_last.y()) * 0.15f;
            params.pitchDeg = clampf(params.pitchDeg, -89.f, 89.f);
            m_yaw += (e->pos().x() - m_last.x()) * 0.003f;
            m_last = e->pos();
            m_dirty = true;
        }
    }

private slots:
    void frame()
    {
        if (m_dirty) {
            QElapsedTimer t; t.start();
            renderSky();
            m_renderMs = float(t.nsecsElapsed()) * 1e-6f;
            m_dirty = false;
            update();
            emit statsChanged();
        }
        if (const char *df = getenv("DUMP_FRAMES")) {
            static int left = atoi(df);
            if (--left <= 0) {
                m_lowres.scaled(m_lowres.width() * params.resScale,
                                m_lowres.height() * params.resScale)
                        .save("dump.png");
                QApplication::quit();
            }
        }
    }

private:
    static int threadCount()
    {
        return std::max(2u, std::thread::hardware_concurrency());
    }

    // in-scattered light along a view ray (single scattering)
    Vec3 inscatter(const Vec3 &orig, const Vec3 &dir, float tMax,
                   const Vec3 &sun, int NS, int NL, Vec3 &transOut) const
    {
        const float betaM = BETA_M * params.turbidity;
        const float g = params.mieG;
        const float mu = dir.dot(sun);
        const float phaseR = 3.f / (16.f * 3.14159265f) * (1.f + mu * mu);
        const float denom = 1.f + g * g - 2.f * g * mu;
        const float phaseM = 3.f / (8.f * 3.14159265f)
            * ((1.f - g * g) * (1.f + mu * mu))
            / ((2.f + g * g) * denom * std::sqrt(std::max(denom, 1e-6f)));

        const float ds = tMax / NS;
        float odR = 0.f, odM = 0.f;
        Vec3 sumR, sumM;

        for (int i = 0; i < NS; ++i) {
            const Vec3 p = orig + dir * ((i + 0.5f) * ds);
            const float h = p.len() - Rg;
            const float rhoR = std::exp(-h / HR) * ds;
            const float rhoM = std::exp(-h / HM) * ds;
            odR += rhoR; odM += rhoM;

            // optical depth toward the sun
            float lt0, lt1;
            raySphere(p, sun, Ra, lt0, lt1);
            float g0, g1;
            if (raySphere(p, sun, Rg, g0, g1) && g0 > 0.f)
                continue; // sample in the planet's shadow
            const float dl = lt1 / NL;
            float lodR = 0.f, lodM = 0.f;
            for (int j = 0; j < NL; ++j) {
                const Vec3 q = p + sun * ((j + 0.5f) * dl);
                const float hh = q.len() - Rg;
                lodR += std::exp(-hh / HR) * dl;
                lodM += std::exp(-hh / HM) * dl;
            }
            const Vec3 tau = BETA_R * (odR + lodR)
                + Vec3(1, 1, 1) * (betaM * 1.1f * (odM + lodM));
            const Vec3 att { std::exp(-tau.x), std::exp(-tau.y),
                             std::exp(-tau.z) };
            sumR = sumR + att * rhoR;
            sumM = sumM + att * rhoM;
        }
        const Vec3 tauView = BETA_R * odR + Vec3(1, 1, 1) * (betaM * 1.1f * odM);
        transOut = { std::exp(-tauView.x), std::exp(-tauView.y),
                     std::exp(-tauView.z) };
        return BETA_R * sumR * phaseR + Vec3(1, 1, 1) * betaM * phaseM * sumM;
    }

    void renderSky()
    {
        static const int SAMPLES[3][2] = { {12, 5}, {20, 8}, {40, 12} };
        const int NS = SAMPLES[params.quality][0];
        const int NL = SAMPLES[params.quality][1];

        const int W2 = std::max(64, width() / params.resScale);
        const int H2 = std::max(48, height() / params.resScale);
        if (m_lowres.width() != W2 || m_lowres.height() != H2)
            m_lowres = QImage(W2, H2, QImage::Format_RGB32);

        const float alt = params.altitudeKm * 1000.f;
        const Vec3 orig { 0.f, Rg + std::max(2.f, alt), 0.f };
        const float se = params.sunElevDeg * 3.14159265f / 180.f;
        const Vec3 sun = Vec3(std::cos(se), std::sin(se), 0.f).norm();

        const float pitch = params.pitchDeg * 3.14159265f / 180.f;
        const float cy = std::cos(m_yaw), sy = std::sin(m_yaw);
        const Vec3 fwd = Vec3(std::cos(pitch) * cy, std::sin(pitch),
                              std::cos(pitch) * sy).norm();
        const Vec3 worldUp { 0.f, 1.f, 0.f };
        Vec3 right = Vec3(fwd.z * worldUp.y - fwd.y * worldUp.z,
                          fwd.x * worldUp.z - fwd.z * worldUp.x,
                          fwd.y * worldUp.x - fwd.x * worldUp.y).norm();
        right = right * -1.f;
        const Vec3 up { right.y * fwd.z - right.z * fwd.y,
                        right.z * fwd.x - right.x * fwd.z,
                        right.x * fwd.y - right.y * fwd.x };

        const float fov = 65.f * 3.14159265f / 180.f;
        const float focal = 0.5f * H2 / std::tan(fov * 0.5f);
        const float expo = params.exposure;

        const int T = threadCount();
        std::vector<std::thread> th;
        for (int t = 0; t < T; ++t) {
            th.emplace_back([&, t] {
                for (int py = t; py < H2; py += T) {
                    QRgb *out = reinterpret_cast<QRgb *>(m_lowres.scanLine(py));
                    for (int px = 0; px < W2; ++px) {
                        const Vec3 dir = (fwd * focal
                            + right * (px - W2 * 0.5f + 0.5f)
                            + up * (H2 * 0.5f - py - 0.5f)).norm();

                        float t0, t1, tg0, tg1;
                        Vec3 col;
                        if (!raySphere(orig, dir, Ra, t0, t1) || t1 < 0.f) {
                            // outside looking away: black space
                        } else {
                            const float tStart = std::max(t0, 0.f);
                            float tEnd = t1;
                            bool ground = raySphere(orig, dir, Rg, tg0, tg1)
                                          && tg0 > 0.f;
                            if (ground) tEnd = tg0;

                            Vec3 trans;
                            col = inscatter(orig + dir * tStart, dir,
                                            tEnd - tStart, sun, NS, NL, trans)
                                  * 20.f;
                            if (ground) {
                                // diffuse ground lit by attenuated sun
                                const Vec3 p = orig + dir * tg0;
                                const Vec3 n = p.norm();
                                const float ndl = std::max(0.f, n.dot(sun));
                                Vec3 st;
                                Vec3 dummy = inscatter(p, sun,
                                    /*to top*/ [&]{ float a,b2;
                                        raySphere(p, sun, Ra, a, b2);
                                        return b2; }(),
                                    sun, 8, 1, st);
                                (void)dummy;
                                const Vec3 albedo { 0.12f, 0.10f, 0.08f };
                                col = col + albedo * ndl * st * trans * 4.f;
                            } else {
                                // sun disk
                                if (dir.dot(sun) > 0.9998f)
                                    col = col + trans * 300.f;
                            }
                        }
                        const float r = 1.f - std::exp(-col.x * expo);
                        const float g = 1.f - std::exp(-col.y * expo);
                        const float b = 1.f - std::exp(-col.z * expo);
                        // cheap gamma
                        out[px] = qRgb(int(std::sqrt(r) * 255),
                                       int(std::sqrt(g) * 255),
                                       int(std::sqrt(b) * 255));
                    }
                }
            });
        }
        for (auto &x : th) x.join();
    }

    QImage m_lowres{320, 200, QImage::Format_RGB32};
    bool m_dirty = true;
    float m_yaw = 0.f;
    float m_renderMs = 0.f;
    QPoint m_last;
    QTimer m_timer;
};

// -------------------------------------------------------------------- window

class MainWindow : public QWidget
{
    Q_OBJECT
public:
    MainWindow()
    {
        setWindowTitle("Skylight — atmospheric scattering");
        auto *canvas = new SkyCanvas;

        // log-scale altitude: 0.1 km .. 40000 km
        auto *alt = new QSlider(Qt::Horizontal);
        alt->setRange(0, 1000);
        auto altToSlider = [](float km) {
            return int(1000.f * std::log(km / 0.1f) / std::log(400000.f));
        };
        auto sliderToAlt = [](int v) {
            return 0.1f * std::pow(400000.f, v / 1000.f);
        };
        alt->setValue(altToSlider(canvas->params.altitudeKm));
        auto *altLabel = new QLabel;

        auto *sunEl = new QSlider(Qt::Horizontal);
        sunEl->setRange(-150, 900); // -15 .. +90 deg, x10
        sunEl->setValue(int(canvas->params.sunElevDeg * 10));
        auto *sunLabel = new QLabel;

        auto *pitch = new QSlider(Qt::Horizontal);
        pitch->setRange(-89, 89);
        pitch->setValue(int(canvas->params.pitchDeg));

        auto *mieG = new QDoubleSpinBox;
        mieG->setRange(0.0, 0.95); mieG->setDecimals(2); mieG->setSingleStep(0.02);
        mieG->setValue(canvas->params.mieG);

        auto *turb = new QDoubleSpinBox;
        turb->setRange(0.2, 8.0); turb->setSingleStep(0.2);
        turb->setValue(canvas->params.turbidity);

        auto *expo = new QDoubleSpinBox;
        expo->setRange(0.1, 8.0); expo->setSingleStep(0.1);
        expo->setValue(canvas->params.exposure);

        auto *quality = new QComboBox;
        quality->addItems({ "Low (12/5)", "Medium (20/8)", "High (40/12)" });
        quality->setCurrentIndex(1);

        auto *res = new QComboBox;
        res->addItems({ "Full res", "1/2", "1/3", "1/4" });
        res->setCurrentIndex(2);

        auto *hint = new QLabel("LMB drag: look around");
        hint->setStyleSheet("color:#889");
        auto *stats = new QLabel;
        stats->setStyleSheet("color:#8fa;font-family:monospace");

        auto *form = new QFormLayout;
        form->addRow("Altitude", alt);
        form->addRow("", altLabel);
        form->addRow("Sun elevation", sunEl);
        form->addRow("", sunLabel);
        form->addRow("Look pitch", pitch);
        form->addRow("Mie g", mieG);
        form->addRow("Turbidity", turb);
        form->addRow("Exposure", expo);
        form->addRow("Quality", quality);
        form->addRow("Resolution", res);
        form->addRow(hint);
        form->addRow(stats);

        auto *group = new QGroupBox("Parameters");
        group->setLayout(form);
        group->setFixedWidth(280);

        auto *layout = new QHBoxLayout(this);
        layout->addWidget(group);
        layout->addWidget(canvas, 1);

        auto updAlt = [canvas, altLabel](float km) {
            canvas->params.altitudeKm = km;
            altLabel->setText(km < 100.f
                ? QString("%1 km").arg(km, 0, 'f', 1)
                : QString("%1 km").arg(km, 0, 'f', 0));
            canvas->invalidate();
        };
        updAlt(canvas->params.altitudeKm);
        sunLabel->setText(QString("%1 deg").arg(canvas->params.sunElevDeg, 0, 'f', 1));

        connect(alt, &QSlider::valueChanged, this, [sliderToAlt, updAlt](int v) {
            updAlt(sliderToAlt(v));
        });
        connect(sunEl, &QSlider::valueChanged, this, [canvas, sunLabel](int v) {
            canvas->params.sunElevDeg = v / 10.f;
            sunLabel->setText(QString("%1 deg").arg(v / 10.f, 0, 'f', 1));
            canvas->invalidate();
        });
        connect(pitch, &QSlider::valueChanged, this, [canvas](int v) {
            canvas->params.pitchDeg = float(v);
            canvas->invalidate();
        });
        connect(mieG, &QDoubleSpinBox::valueChanged, this, [canvas](double v) {
            canvas->params.mieG = float(v); canvas->invalidate();
        });
        connect(turb, &QDoubleSpinBox::valueChanged, this, [canvas](double v) {
            canvas->params.turbidity = float(v); canvas->invalidate();
        });
        connect(expo, &QDoubleSpinBox::valueChanged, this, [canvas](double v) {
            canvas->params.exposure = float(v); canvas->invalidate();
        });
        connect(quality, &QComboBox::currentIndexChanged, this, [canvas](int i) {
            canvas->params.quality = i; canvas->invalidate();
        });
        connect(res, &QComboBox::currentIndexChanged, this, [canvas](int i) {
            canvas->params.resScale = i + 1; canvas->invalidate();
        });
        connect(canvas, &SkyCanvas::statsChanged, this, [canvas, stats] {
            stats->setText(QString("render %1 ms")
                           .arg(canvas->lastRenderMs(), 0, 'f', 0));
        });

        resize(1280, 800);
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette pal;
    pal.setColor(QPalette::Window, QColor(37, 37, 42));
    pal.setColor(QPalette::WindowText, QColor(220, 220, 224));
    pal.setColor(QPalette::Base, QColor(28, 28, 32));
    pal.setColor(QPalette::Text, QColor(220, 220, 224));
    pal.setColor(QPalette::Button, QColor(48, 48, 54));
    pal.setColor(QPalette::ButtonText, QColor(220, 220, 224));
    pal.setColor(QPalette::Highlight, QColor(70, 120, 200));
    app.setPalette(pal);

    MainWindow w;
    w.show();
    return app.exec();
}

#include "main.moc"
