#include "VideoGLWidget.hpp"
#include "RtspSource.hpp"

#include <QDateTime>
#include <QDebug>
#include <QOpenGLContext>
#include <QPainter>
#include <QPen>

#include <cuda_runtime.h>

namespace {

const char* kVertSrc = R"(
#version 120
attribute vec2 aPos;
attribute vec2 aUV;
varying vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

const char* kFragSrc = R"(
#version 120
uniform sampler2D uTex;
varying vec2 vUV;
void main() {
    gl_FragColor = texture2D(uTex, vUV);
}
)";

bool cudaOk(cudaError_t err, const char* what) {
    if (err == cudaSuccess) return true;
    qWarning().noquote() << QString("%1 failed: %2").arg(what, cudaGetErrorString(err));
    return false;
}

}  // namespace

VideoGLWidget::VideoGLWidget(QWidget* parent) : QOpenGLWidget(parent) {}

VideoGLWidget::~VideoGLWidget() {
    makeCurrent();
    releaseCudaInterop();
    delete m_program;
    if (m_texture) glDeleteTextures(1, &m_texture);
    if (m_vbo)     glDeleteBuffers(1, &m_vbo);
    if (m_vao.isCreated()) m_vao.destroy();
    delete m_currentHolder;
    delete m_nextHolder;
    doneCurrent();
}

void VideoGLWidget::setStatusLine(QString text) {
    m_statusLine = std::move(text);
}

void VideoGLWidget::releaseCudaInterop() {
    if (m_cudaGlResource) {
        cudaGraphicsUnregisterResource(m_cudaGlResource);
        m_cudaGlResource = nullptr;
    }
}

bool VideoGLWidget::ensureTextureForFrame(const FrameHolder& holder) {
    if (holder.width == 0 || holder.height == 0) {
        qWarning() << "Frame has invalid dimensions:" << holder.width << holder.height;
        return false;
    }

    if (m_textureWidth == int(holder.width) &&
        m_textureHeight == int(holder.height) &&
        m_cudaGlResource) {
        return true;
    }

    releaseCudaInterop();

    m_textureWidth  = int(holder.width);
    m_textureHeight = int(holder.height);

    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                 m_textureWidth, m_textureHeight,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    if (!cudaOk(cudaGraphicsGLRegisterImage(
            &m_cudaGlResource,
            m_texture,
            GL_TEXTURE_2D,
            cudaGraphicsRegisterFlagsWriteDiscard),
            "cudaGraphicsGLRegisterImage")) {
        m_cudaGlResource = nullptr;
        return false;
    }

    qInfo().noquote() << QString("Registered GL texture with CUDA: %1x%2")
        .arg(m_textureWidth).arg(m_textureHeight);
    return true;
}

bool VideoGLWidget::uploadFrameToTexture(const FrameHolder& holder) {
    if (!ensureTextureForFrame(holder)) {
        return false;
    }

    if (!cudaOk(cudaGraphicsMapResources(1, &m_cudaGlResource, 0),
                "cudaGraphicsMapResources")) {
        return false;
    }

    cudaArray_t array = nullptr;
    bool ok = cudaOk(cudaGraphicsSubResourceGetMappedArray(&array, m_cudaGlResource, 0, 0),
                     "cudaGraphicsSubResourceGetMappedArray");
    if (ok) {
        const void* src = holder.surface->surfaceList[0].dataPtr;
        ok = cudaOk(cudaMemcpy2DToArray(
                        array,
                        0,
                        0,
                        src,
                        holder.pitch,
                        holder.width * 4u,
                        holder.height,
                        cudaMemcpyDeviceToDevice),
                    "cudaMemcpy2DToArray");
    }

    if (!cudaOk(cudaGraphicsUnmapResources(1, &m_cudaGlResource, 0),
                "cudaGraphicsUnmapResources")) {
        ok = false;
    }

    return ok;
}

void VideoGLWidget::initializeGL() {
    initializeOpenGLFunctions();

    const QSurfaceFormat fmt = context()->format();
    qInfo().noquote() << "GL_VERSION       :" << reinterpret_cast<const char*>(glGetString(GL_VERSION));
    qInfo().noquote() << "GL_RENDERER      :" << reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* profile =
        fmt.profile() == QSurfaceFormat::CoreProfile          ? "Core" :
        fmt.profile() == QSurfaceFormat::CompatibilityProfile ? "Compatibility" : "NoProfile";
    qInfo().noquote() << "Qt profile/ver   :" << profile
                      << QString("%1.%2").arg(fmt.majorVersion()).arg(fmt.minorVersion());

    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

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

    if (!m_vao.create()) {
        qFatal("VAO creation failed");
    }
    m_vao.bind();

    static const float quad[] = {
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
    };
    glGenBuffers(1, &m_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    const GLint posLoc = m_program->attributeLocation("aPos");
    const GLint uvLoc  = m_program->attributeLocation("aUV");
    glEnableVertexAttribArray(posLoc);
    glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(float),
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(uvLoc);
    glVertexAttribPointer(uvLoc, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));

    m_vao.release();

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    m_statsTimer.start();
}

void VideoGLWidget::resizeGL(int w, int h) {
    glViewport(0, 0, w, h);
}

void VideoGLWidget::paintGL() {
    glClear(GL_COLOR_BUFFER_BIT);

    if (m_nextHolder) {
        if (uploadFrameToTexture(*m_nextHolder)) {
            delete m_currentHolder;
            m_currentHolder = m_nextHolder;
        } else {
            delete m_nextHolder;
        }
        m_nextHolder = nullptr;
    }

    if (m_currentHolder) {
        m_program->bind();
        m_vao.bind();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_texture);
        m_program->setUniformValue("uTex", 0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        m_vao.release();
        m_program->release();

        const qint64 nowNs = QDateTime::currentMSecsSinceEpoch() * 1000000LL;
        const qint64 latMs = (nowNs - m_currentHolder->captureWallNs) / 1000000;
        m_latencySumMs += latMs;
        if (latMs > m_latencyMaxMs) m_latencyMaxMs = latMs;
        ++m_frameCount;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setPen(QPen(QColor("#008cff"), 1));
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
    painter.fillRect(QRect(8, 8, width() - 16, 28), QColor(0, 0, 0, 160));
    painter.setPen(Qt::white);
    painter.drawText(QRect(16, 12, width() - 32, 20), Qt::AlignLeft | Qt::AlignVCenter, m_statusLine);
    painter.end();

    if (m_statsTimer.elapsed() >= 1000) {
        const double avg = m_frameCount ? double(m_latencySumMs) / m_frameCount : 0.0;
        qInfo().noquote() << QString(
            "fps=%1 ingest-to-paint avg=%2 ms max=%3 ms | tex=%4x%5 | %6")
            .arg(m_frameCount)
            .arg(avg, 0, 'f', 1)
            .arg(m_latencyMaxMs)
            .arg(m_textureWidth)
            .arg(m_textureHeight)
            .arg(m_statusLine);

        m_frameCount   = 0;
        m_latencySumMs = 0;
        m_latencyMaxMs = 0;
        m_statsTimer.restart();
    }
}

void VideoGLWidget::onNewFrame(FrameHolder* holder) {
    if (m_nextHolder) {
        delete m_nextHolder;
    }
    m_nextHolder = holder;
    update();
}
