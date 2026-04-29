#include <QApplication>
#include <QSurfaceFormat>
#include <QString>
#include <gst/gst.h>

#include "GstPipeline.hpp"
#include "VideoGLWidget.hpp"

namespace {

// Default pipeline: synthetic NVMM source. No camera, no DeepStream pipeline
// elements, just nvvideoconvert producing an NV12 NVMM buffer for appsink.
// Lets us isolate the EGLImage/Qt path from any RTSP or decoder issue.
constexpr const char* kDefaultPipeline =
    "videotestsrc is-live=true pattern=ball ! "
    "video/x-raw,width=1920,height=1080,framerate=30/1 ! "
    "nvvideoconvert ! "
    "video/x-raw(memory:NVMM),format=RGBA ! "
    "appsink name=sink";

}  // namespace

int main(int argc, char* argv[]) {
    // Force EGL for Qt's GL context. Without this, on X11 Qt would create a
    // GLX context whose EGLDisplay is not the one NvBufSurface uses, and
    // glEGLImageTargetTexture2DOES would either not resolve or render black.
    //
    // DIAGNOSTIC: allow ALLOW_GLX=1 to skip this and let Qt pick the default
    // GLX integration. The diagnostic gradient shader doesn't touch the
    // texture path so it should render identically; if it does NOT (i.e.
    // window stays black under EGL but turns colorful under GLX), the bug
    // is in the EGL <-> QOpenGLWidget composition, not in our GL code.
    if (qgetenv("ALLOW_GLX") != "1") {
        qputenv("QT_XCB_GL_INTEGRATION", "xcb_egl");
    }

    gst_init(&argc, &argv);

    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setMajorVersion(3);
    fmt.setMinorVersion(3);
    // Compatibility profile is required so the GL_OES_EGL_image_external
    // extension and samplerExternalOES are usable on desktop NVIDIA GL.
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
