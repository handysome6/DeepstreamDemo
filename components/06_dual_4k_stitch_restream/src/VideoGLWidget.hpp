#pragma once

#include <QElapsedTimer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QString>

#include <cuda_gl_interop.h>

struct StitchedFrame;

class VideoGLWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    explicit VideoGLWidget(QWidget* parent = nullptr);
    ~VideoGLWidget() override;

    void setStatusLine(QString text);

public slots:
    void onNewFrame(StitchedFrame* holder);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    void releaseCudaInterop();
    bool ensureTextureForFrame(const StitchedFrame& holder);
    bool uploadFrameToTexture(const StitchedFrame& holder);

    GLuint m_texture = 0;
    GLuint m_vbo     = 0;
    QOpenGLVertexArrayObject m_vao;
    QOpenGLShaderProgram* m_program = nullptr;
    cudaGraphicsResource* m_cudaGlResource = nullptr;
    int m_textureWidth  = 0;
    int m_textureHeight = 0;

    StitchedFrame* m_nextHolder    = nullptr;
    StitchedFrame* m_currentHolder = nullptr;

    QElapsedTimer m_statsTimer;
    int           m_frameCount       = 0;
    qint64        m_latencySumMs     = 0;
    qint64        m_latencyMaxMs     = 0;

    QString m_statusLine;
};
