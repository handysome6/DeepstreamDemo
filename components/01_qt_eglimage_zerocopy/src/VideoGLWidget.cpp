#include "VideoGLWidget.hpp"
#include "GstPipeline.hpp"

#include <QDateTime>
#include <QDebug>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif

// glEGLImageTargetTexture2DOES is an extension entry point; resolve at init.
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC s_glEGLImageTargetTexture2DOES = nullptr;

// GLSL 1.20 + GL_OES_EGL_image_external. Works in NVIDIA desktop GL
// compatibility profile and matches the EGLImage produced by
// NvBufSurfaceMapEglImage on dGPU.
static const char* kVertSrc = R"(
#version 120
attribute vec2 aPos;
attribute vec2 aUV;
varying   vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

static const char* kFragSrc = R"(
#version 120
#extension GL_OES_EGL_image_external : require
uniform samplerExternalOES uTex;
varying vec2 vUV;
void main() {
    gl_FragColor = texture2D(uTex, vUV);
}
)";

VideoGLWidget::VideoGLWidget(QWidget* parent) : QOpenGLWidget(parent) {}

VideoGLWidget::~VideoGLWidget() {
    makeCurrent();
    delete m_program;
    if (m_texture) glDeleteTextures(1, &m_texture);
    if (m_vbo)     glDeleteBuffers(1, &m_vbo);
    delete m_currentHolder;
    delete m_nextHolder;
    doneCurrent();
}

void VideoGLWidget::initializeGL() {
    initializeOpenGLFunctions();

    s_glEGLImageTargetTexture2DOES =
        reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!s_glEGLImageTargetTexture2DOES) {
        qFatal("glEGLImageTargetTexture2DOES not available. "
               "Is Qt using EGL (QT_XCB_GL_INTEGRATION=xcb_egl)?");
    }

    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_texture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);

    m_program = new QOpenGLShaderProgram(this);
    if (!m_program->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertSrc)) {
        qFatal("Vertex shader compile failed:\n%s", qPrintable(m_program->log()));
    }
    if (!m_program->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragSrc)) {
        qFatal("Fragment shader compile failed:\n%s", qPrintable(m_program->log()));
    }
    if (!m_program->link()) {
        qFatal("Shader link failed:\n%s", qPrintable(m_program->log()));
    }

    // Fullscreen quad. UV is flipped on Y so the picture appears upright;
    // GStreamer frames are top-down and GL textures are bottom-up.
    static const float quad[] = {
        // pos        uv
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
    };
    glGenBuffers(1, &m_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    m_statsTimer.start();
}

void VideoGLWidget::resizeGL(int w, int h) {
    glViewport(0, 0, w, h);
}

void VideoGLWidget::paintGL() {
    glClear(GL_COLOR_BUFFER_BIT);

    if (m_nextHolder) {
        // Re-bind the texture to the new EGLImage. After this call the
        // previous EGLImage is no longer referenced by m_texture, so the
        // previous holder is safe to release.
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_texture);
        s_glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES,
                                       m_nextHolder->eglImage);

        delete m_currentHolder;
        m_currentHolder = m_nextHolder;
        m_nextHolder    = nullptr;
    }

    if (!m_currentHolder) return;

    m_program->bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_texture);
    m_program->setUniformValue("uTex", 0);

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    const int posLoc = m_program->attributeLocation("aPos");
    const int uvLoc  = m_program->attributeLocation("aUV");
    glEnableVertexAttribArray(posLoc);
    glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(float),
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(uvLoc);
    glVertexAttribPointer(uvLoc, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(posLoc);
    glDisableVertexAttribArray(uvLoc);
    m_program->release();

    // Latency stats: time from new-sample callback to end of paintGL.
    const qint64 nowNs = QDateTime::currentMSecsSinceEpoch() * 1000000LL;
    const qint64 latMs = (nowNs - m_currentHolder->captureWallNs) / 1000000;
    m_latencySumMs += latMs;
    if (latMs > m_latencyMaxMs) m_latencyMaxMs = latMs;
    ++m_frameCount;

    if (m_statsTimer.elapsed() >= 1000) {
        const double avg = m_frameCount ? double(m_latencySumMs) / m_frameCount : 0.0;
        qInfo().noquote() << QString(
            "fps=%1  ingest-to-paint avg=%2 ms  max=%3 ms")
            .arg(m_frameCount).arg(avg, 0, 'f', 1).arg(m_latencyMaxMs);
        m_frameCount   = 0;
        m_latencySumMs = 0;
        m_latencyMaxMs = 0;
        m_statsTimer.restart();
    }
}

void VideoGLWidget::onNewFrame(FrameHolder* holder) {
    // Producer is faster than the GL thread: drop the queued frame.
    if (m_nextHolder) {
        delete m_nextHolder;
    }
    m_nextHolder = holder;
    update();
}
