#pragma once

#include <QElapsedTimer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QString>

#include <cuda_gl_interop.h>

struct FrameHolder;

// dGPU GPU-only display widget, duplicated from component 01. Per the
// architecture rule, no shared code goes into a `common/` directory until a
// third component asks for the same thing.
class VideoGLWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    explicit VideoGLWidget(QWidget* parent = nullptr);
    ~VideoGLWidget() override;

    // Set by main() / the source from outside; rendered as a small overlay
    // line so a human watching the window can verify reconnect behavior
    // without tailing stdout.
    void setStatusLine(QString text);

public slots:
    void onNewFrame(FrameHolder* holder);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    void releaseCudaInterop();
    bool ensureTextureForFrame(const FrameHolder& holder);
    bool uploadFrameToTexture(const FrameHolder& holder);

    GLuint m_texture = 0;
    GLuint m_vbo     = 0;
    QOpenGLVertexArrayObject m_vao;
    QOpenGLShaderProgram* m_program = nullptr;
    cudaGraphicsResource* m_cudaGlResource = nullptr;
    int m_textureWidth  = 0;
    int m_textureHeight = 0;

    FrameHolder* m_nextHolder    = nullptr;
    FrameHolder* m_currentHolder = nullptr;

    QElapsedTimer m_statsTimer;
    int           m_frameCount       = 0;
    qint64        m_latencySumMs     = 0;
    qint64        m_latencyMaxMs     = 0;

    QString m_statusLine;
};
