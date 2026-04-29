#include <QApplication>
#include <QSurfaceFormat>
#include <QString>
#include <gst/gst.h>

#include "GstPipeline.hpp"
#include "VideoGLWidget.hpp"

namespace {

// Default pipeline: synthetic NVMM source. No camera, no DeepStream pipeline
// elements, just nvvideoconvert producing an RGBA CUDA-device buffer for
// appsink. Lets us isolate the dGPU GPU-only Qt display path from any RTSP
// or decoder issue.
constexpr const char* kDefaultPipeline =
    "videotestsrc is-live=true pattern=ball ! "
    "video/x-raw,width=1920,height=1080,framerate=30/1 ! "
    "nvvideoconvert ! "
    "video/x-raw(memory:NVMM),format=RGBA ! "
    "appsink name=sink";

}  // namespace

int main(int argc, char* argv[]) {
    // On this DS9.0 + NVIDIA 580 + Qt6 stack, QOpenGLWidget composes to a
    // visible X11 window correctly under xcb_glx but renders a black window
    // under xcb_egl even when the internal FBO contents are correct.
    //
    // Default to GLX for the dGPU GPU-only display path. Keep ALLOW_EGL=1 as
    // an escape hatch for diagnostics and for future regression checks against
    // the known-bad compose path.
    if (qgetenv("ALLOW_EGL") == "1") {
        qputenv("QT_XCB_GL_INTEGRATION", "xcb_egl");
    } else {
        qputenv("QT_XCB_GL_INTEGRATION", "xcb_glx");
    }

    gst_init(&argc, &argv);

    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setMajorVersion(3);
    fmt.setMinorVersion(3);
    // Compatibility profile keeps the Qt/OpenGL setup aligned with the known
    // good desktop NVIDIA path while we render a regular GL_TEXTURE_2D.
    fmt.setProfile(QSurfaceFormat::CompatibilityProfile);
    // Disable vsync so the latency probe measures the render path itself,
    // not the swap-interval wait.
    fmt.setSwapInterval(0);
    QSurfaceFormat::setDefaultFormat(fmt);

    QApplication app(argc, argv);
    qRegisterMetaType<FrameHolder*>("FrameHolder*");

    const QString pipelineDesc = (argc > 1)
        ? QString::fromUtf8(argv[1])
        : QString::fromUtf8(kDefaultPipeline);

    VideoGLWidget widget;
    widget.resize(1280, 720);
    widget.setWindowTitle("01_qt_eglimage_zerocopy");
    widget.show();

    GstSourcePipeline pipeline(pipelineDesc.toStdString());
    QObject::connect(&pipeline, &GstSourcePipeline::newFrame,
                     &widget,   &VideoGLWidget::onNewFrame,
                     Qt::QueuedConnection);
    QObject::connect(&pipeline, &GstSourcePipeline::pipelineError,
                     &app, [](const QString& msg) {
                         qWarning() << "Pipeline error:" << msg;
                         QCoreApplication::quit();
                     });

    if (!pipeline.start()) {
        return 2;
    }

    const int rc = app.exec();
    pipeline.stop();
    return rc;
}
