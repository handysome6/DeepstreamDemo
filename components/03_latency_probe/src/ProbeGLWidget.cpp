#include "ProbeGLWidget.hpp"
#include "ProbeSource.hpp"
#include "LatencyAggregator.hpp"

#include <QDebug>
#include <QOpenGLContext>
#include <QPainter>

#include <cuda_runtime.h>
#include <glib.h>

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

qint64 monotonicNowUs() {
    return qint64(g_get_monotonic_time());
}

}  // namespace

ProbeGLWidget::ProbeGLWidget(LatencyAggregator* agg, QWidget* parent)
    : QOpenGLWidget(parent), m_agg(agg) {
    connect(this, &QOpenGLWidget::frameSwapped,
            this, &ProbeGLWidget::onFrameSwapped);
}

ProbeGLWidget::~ProbeGLWidget() {
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

void ProbeGLWidget::setStatusLine(QString text) {
    m_statusLine = std::move(text);
}

void ProbeGLWidget::releaseCudaInterop() {
    if (m_cudaGlResource) {
        cudaGraphicsUnregisterResource(m_cudaGlResource);
        m_cudaGlResource = nullptr;
    }
}

bool ProbeGLWidget::ensureTextureForFrame(const FrameHolder& holder) {
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
            &m_cudaGlResource, m_texture, GL_TEXTURE_2D,
            cudaGraphicsRegisterFlagsWriteDiscard),
            "cudaGraphicsGLRegisterImage")) {
        m_cudaGlResource = nullptr;
        return false;
    }
    qInfo().noquote() << QString("Registered GL texture with CUDA: %1x%2")
        .arg(m_textureWidth).arg(m_textureHeight);
    return true;
}

bool ProbeGLWidget::uploadFrameToTexture(const FrameHolder& holder) {
    if (!ensureTextureForFrame(holder)) return false;
    if (!cudaOk(cudaGraphicsMapResources(1, &m_cudaGlResource, 0),
                "cudaGraphicsMapResources")) {
        return false;
    }
    cudaArray_t array = nullptr;
    bool ok = cudaOk(cudaGraphicsSubResourceGetMappedArray(&array, m_cudaGlResource, 0, 0),
                     "cudaGraphicsSubResourceGetMappedArray");
    if (ok) {
        const void* src = holder.surface->surfaceList[0].dataPtr;
        ok = cudaOk(cudaMemcpy2DToArray(array, 0, 0, src, holder.pitch,
                                        holder.width * 4u, holder.height,
                                        cudaMemcpyDeviceToDevice),
                    "cudaMemcpy2DToArray");
    }
    if (!cudaOk(cudaGraphicsUnmapResources(1, &m_cudaGlResource, 0),
                "cudaGraphicsUnmapResources")) {
        ok = false;
    }
    return ok;
}

void ProbeGLWidget::initializeGL() {
    initializeOpenGLFunctions();

    const QSurfaceFormat fmt = context()->format();
    qInfo().noquote() << "GL_VERSION    :"
        << reinterpret_cast<const char*>(glGetString(GL_VERSION));
    qInfo().noquote() << "GL_RENDERER   :"
        << reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    qInfo().noquote() << "Qt swapInterval:" << fmt.swapInterval();

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

    if (!m_vao.create()) qFatal("VAO creation failed");
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
                          4 * sizeof(float), reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(uvLoc);
    glVertexAttribPointer(uvLoc, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));

    m_vao.release();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

void ProbeGLWidget::resizeGL(int w, int h) {
    glViewport(0, 0, w, h);
}

void ProbeGLWidget::paintGL() {
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

        // Stash this frame's t0/t1 for onFrameSwapped to consume — sampling
        // t2 here would miss the swap-on-vblank wait that the README's
        // vsync sanity criterion specifically targets.
        m_pendingT0_us = m_currentHolder->t0_us;
        m_pendingT1_us = m_currentHolder->t1_us;
    } else {
        // No frame this paint => no sample to attribute. Make sure a stale
        // pair doesn't get re-emitted by the next swap.
        m_pendingT0_us = 0;
        m_pendingT1_us = 0;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.fillRect(QRect(8, 8, width() - 16, 28), QColor(0, 0, 0, 160));
    painter.setPen(Qt::white);
    painter.drawText(QRect(16, 12, width() - 32, 20),
                     Qt::AlignLeft | Qt::AlignVCenter, m_statusLine);
    painter.end();
}

void ProbeGLWidget::onFrameSwapped() {
    if (m_pendingT0_us == 0 || m_pendingT1_us == 0) return;
    const qint64 t2_us = monotonicNowUs();
    const qint64 t0 = m_pendingT0_us;
    const qint64 t1 = m_pendingT1_us;
    m_pendingT0_us = 0;
    m_pendingT1_us = 0;
    if (m_agg && t1 >= t0 && t2_us >= t1) {
        m_agg->addSample(/*decode_queue*/ t1 - t0,
                         /*upload_paint*/ t2_us - t1,
                         /*end_to_end  */ t2_us - t0);
    }
}

void ProbeGLWidget::onNewFrame(FrameHolder* holder) {
    if (m_nextHolder) {
        delete m_nextHolder;
    }
    m_nextHolder = holder;
    update();
}
