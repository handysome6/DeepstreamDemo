#pragma once

#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QString>

#include <cuda_gl_interop.h>

struct FrameHolder;
class LatencyAggregator;

// Same dGPU GPU-only display widget as P0.1/P0.2, with one critical
// difference: t2 is sampled in QOpenGLWidget::frameSwapped — i.e. *after*
// the buffer has been swapped (and, with vsync on, after the swap has
// blocked on vblank). Sampling earlier (e.g. after glFinish in paintGL)
// would miss the vsync wait, defeating one of the README success criteria.
//
// On every successfully swapped frame, the widget computes:
//   decode_queue = t1 - t0   (stamped by ProbeSource)
//   upload_paint = t2 - t1   (CUDA->GL upload + draw + swap, incl. vsync wait)
//   end_to_end   = t2 - t0
// and forwards them to the LatencyAggregator passed at construction.
class ProbeGLWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    explicit ProbeGLWidget(LatencyAggregator* agg, QWidget* parent = nullptr);
    ~ProbeGLWidget() override;

    void setStatusLine(QString text);

public slots:
    void onNewFrame(FrameHolder* holder);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private slots:
    // Wired to QOpenGLWidget::frameSwapped in the constructor. Fires after
    // Qt has actually swapped the back buffer, so the t2 sampled here
    // includes the vsync wait when QSurfaceFormat::setSwapInterval(1).
    void onFrameSwapped();

private:
    void releaseCudaInterop();
    bool ensureTextureForFrame(const FrameHolder& holder);
    bool uploadFrameToTexture(const FrameHolder& holder);

    LatencyAggregator* m_agg = nullptr;

    GLuint m_texture = 0;
    GLuint m_vbo     = 0;
    QOpenGLVertexArrayObject m_vao;
    QOpenGLShaderProgram* m_program = nullptr;
    cudaGraphicsResource* m_cudaGlResource = nullptr;
    int m_textureWidth  = 0;
    int m_textureHeight = 0;

    FrameHolder* m_nextHolder    = nullptr;
    FrameHolder* m_currentHolder = nullptr;

    // Carried from paintGL to onFrameSwapped so the post-swap t2 can be
    // attributed to the frame that was just drawn. Reset to 0 once consumed
    // so a stray frameSwapped (e.g. from a paintGL with no holder) is a
    // no-op rather than a bogus sample.
    qint64 m_pendingT0_us = 0;
    qint64 m_pendingT1_us = 0;

    QString m_statusLine;
};
