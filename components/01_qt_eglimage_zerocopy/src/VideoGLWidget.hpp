#pragma once

#include <QElapsedTimer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>

#include <cuda_gl_interop.h>

struct FrameHolder;

class VideoGLWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    explicit VideoGLWidget(QWidget* parent = nullptr);
    ~VideoGLWidget() override;

public slots:
    // Receiver of frames from GstPipeline. Takes ownership of holder.
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

    // Two-slot pipeline:
    //   m_nextHolder    : freshly arrived frame, not yet copied into GL.
    //   m_currentHolder : last frame successfully copied into m_texture.
    FrameHolder* m_nextHolder    = nullptr;
    FrameHolder* m_currentHolder = nullptr;

    QElapsedTimer m_statsTimer;
    int           m_frameCount       = 0;
    qint64        m_latencySumMs     = 0;
    qint64        m_latencyMaxMs     = 0;
};
