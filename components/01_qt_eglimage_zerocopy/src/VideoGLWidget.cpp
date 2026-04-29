#include "VideoGLWidget.hpp"
#include "GstPipeline.hpp"

#include <QDateTime>
#include <QDebug>
#include <QOpenGLContext>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif

// glEGLImageTargetTexture2DOES is an extension entry point; resolve at init.
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC s_glEGLImageTargetTexture2DOES = nullptr;

// GLSL 330 core. We pin attribute locations explicitly so attributeLocation
// lookups can never fail, and use a named fragment output instead of the
// deprecated gl_FragColor. This works whether Qt actually gave us Core or
// Compatibility profile; OES sampling, when we add it back, can come in a
// separate fragment-shader variant.
static const char* kVertSrc = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// DIAGNOSTIC SHADER: ignore the texture entirely and output UV as a gradient.
// Visible result expectation: left edge red->black goes red, top->bottom
// green; corners: bottom-left=black, bottom-right=red, top-left=green,
// top-right=yellow.
static const char* kFragSrc = R"(
#version 330 core
in  vec2 vUV;
out vec4 fragColor;
void main() {
    fragColor = vec4(vUV.x, vUV.y, 0.0, 1.0);
}
)";

VideoGLWidget::VideoGLWidget(QWidget* parent) : QOpenGLWidget(parent) {}

VideoGLWidget::~VideoGLWidget() {
    makeCurrent();
    delete m_program;
    if (m_texture) glDeleteTextures(1, &m_texture);
    if (m_vbo)     glDeleteBuffers(1, &m_vbo);
    if (m_vao.isCreated()) m_vao.destroy();
    delete m_currentHolder;
    delete m_nextHolder;
    doneCurrent();
}

void VideoGLWidget::initializeGL() {
    initializeOpenGLFunctions();

    // ── Diagnostic: dump what context Qt actually gave us. ────────────────
    const QSurfaceFormat fmt = context()->format();
    qInfo().noquote() << "GL_VERSION       :" << reinterpret_cast<const char*>(glGetString(GL_VERSION));
    qInfo().noquote() << "GL_RENDERER      :" << reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    qInfo().noquote() << "GL_SHADING_LANG  :" << reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
    const char* profile =
        fmt.profile() == QSurfaceFormat::CoreProfile          ? "Core" :
        fmt.profile() == QSurfaceFormat::CompatibilityProfile ? "Compatibility" : "NoProfile";
    qInfo().noquote() << "Qt profile/ver   :" << profile
                      << QString("%1.%2").arg(fmt.majorVersion()).arg(fmt.minorVersion());

    s_glEGLImageTargetTexture2DOES =
        reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!s_glEGLImageTargetTexture2DOES) {
        qWarning("glEGLImageTargetTexture2DOES not resolved (texture path will fail)");
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
    if (!m_program->log().isEmpty()) {
        qInfo().noquote() << "Shader log       :" << m_program->log();
    }

    // VAO + VBO + attribute layout: set up once, replay on every paintGL.
    if (!m_vao.create()) {
        qFatal("VAO creation failed");
    }
    m_vao.bind();

    // Fullscreen quad (TRIANGLE_STRIP). UV's Y is flipped because frame
    // memory is top-down, GL is bottom-up.
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

    constexpr GLuint kPosLoc = 0;  // matches layout(location=0)
    constexpr GLuint kUVLoc  = 1;  // matches layout(location=1)
    glEnableVertexAttribArray(kPosLoc);
    glVertexAttribPointer(kPosLoc, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(float),
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(kUVLoc);
    glVertexAttribPointer(kUVLoc, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));

    m_vao.release();

    // Diagnostic background color: deep purple. If you see purple but no
    // gradient quad, glClear works and the draw call is the problem.
    // If you see all-black, even glClear isn't reaching the framebuffer.
    glClearColor(0.2f, 0.0f, 0.3f, 1.0f);
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
        if (s_glEGLImageTargetTexture2DOES) {
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_texture);
            s_glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES,
                                           m_nextHolder->eglImage);
        }

        delete m_currentHolder;
        m_currentHolder = m_nextHolder;
        m_nextHolder    = nullptr;
    }

    // Diagnostic mode: draw the gradient quad regardless of whether a
    // frame has arrived, so we can isolate the GL pipeline from the
    // EGLImage path. Once GL is proven, we'll gate this on m_currentHolder.
    m_program->bind();
    m_vao.bind();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_vao.release();
    m_program->release();

    if (!m_currentHolder) {
        // Still bump the frame stats so we see paintGL is alive even with
        // no frames yet.
        ++m_frameCount;
        if (m_statsTimer.elapsed() >= 1000) {
            qInfo().noquote() << QString("paintGL ticks=%1 (no frames yet)").arg(m_frameCount);
            m_frameCount = 0;
            m_statsTimer.restart();
        }
        return;
    }

    // Latency stats: time from new-sample callback to end of paintGL.
    const qint64 nowNs = QDateTime::currentMSecsSinceEpoch() * 1000000LL;
    const qint64 latMs = (nowNs - m_currentHolder->captureWallNs) / 1000000;
    m_latencySumMs += latMs;
    if (latMs > m_latencyMaxMs) m_latencyMaxMs = latMs;
    ++m_frameCount;

    if (m_statsTimer.elapsed() >= 1000) {
        const double avg = m_frameCount ? double(m_latencySumMs) / m_frameCount : 0.0;

        // FBO read-back probe: sample 5 spots on the QOpenGLWidget's internal
        // FBO. Coords are in *FBO* pixels (y is bottom-up). If GL is drawing
        // correctly, we expect:
        //   center      ≈ (128,128,  0) gradient mid
        //   bottom-left ≈ (  0,  0,  0) (R=u=0, G=v=0 — quad bottom-left)
        //   bottom-right≈ (255,  0,  0) (u=1, v=0)
        //   top-left    ≈ (  0,255,  0)
        //   top-right   ≈ (255,255,  0)
        // If we instead see (51,0,77) (≈ purple clear) the draw didn't run.
        // If we see (0,0,0) the clear didn't run either → FBO/context broken.
        const int W = width()  > 0 ? width()  : 1;
        const int H = height() > 0 ? height() : 1;
        struct Pt { const char* name; int x, y; };
        const Pt pts[] = {
            {"center", W/2, H/2},
            {"BL",     2,   2},
            {"BR",     W-3, 2},
            {"TL",     2,   H-3},
            {"TR",     W-3, H-3},
        };
        QString probe;
        for (const auto& p : pts) {
            unsigned char rgba[4]{};
            glReadPixels(p.x, p.y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            probe += QString(" %1=(%2,%3,%4)")
                .arg(p.name).arg(rgba[0]).arg(rgba[1]).arg(rgba[2]);
        }
        qInfo().noquote() << QString(
            "fps=%1  ingest-to-paint avg=%2 ms  max=%3 ms | FBO probe:%4")
            .arg(m_frameCount).arg(avg, 0, 'f', 1).arg(m_latencyMaxMs).arg(probe);

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
