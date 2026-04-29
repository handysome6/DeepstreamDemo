#pragma once

#include <QElapsedTimer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>

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
    GLuint m_texture = 0;
    GLuint m_vbo     = 0;
    QOpenGLVertexArrayObject m_vao;        // mandatory in Core profile
    QOpenGLShaderProgram* m_program = nullptr;

    // Two-slot pipeline:
    //   m_nextHolder    : freshly arrived frame, not yet bound to GL.
    //   m_currentHolder : last frame whose EGLImage is currently bound to
    //                     m_texture. Released only after the next frame
    //                     overwrites the binding in paintGL.
    FrameHolder* m_nextHolder    = nullptr;
    FrameHolder* m_currentHolder = nullptr;

    QElapsedTimer m_statsTimer;
    int           m_frameCount       = 0;
    qint64        m_latencySumMs     = 0;
    qint64        m_latencyMaxMs     = 0;
};
