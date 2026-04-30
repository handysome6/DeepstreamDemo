#include "LatencyAggregator.hpp"

#include <QDebug>
#include <QFileInfo>
#include <QDir>

#include <glib.h>

#include <algorithm>
#include <vector>

namespace {

qint64 monotonicNowUs() {
    return qint64(g_get_monotonic_time());
}

qint64 percentile(const std::vector<qint64>& sorted, double p) {
    if (sorted.empty()) return 0;
    const double idx = p * (sorted.size() - 1);
    const size_t lo = size_t(std::floor(idx));
    const size_t hi = size_t(std::ceil(idx));
    if (lo == hi) return sorted[lo];
    const double frac = idx - lo;
    return qint64(sorted[lo] * (1.0 - frac) + sorted[hi] * frac);
}

QString formatMs(qint64 us) {
    return QString::number(us / 1000.0, 'f', 2);
}

}  // namespace

LatencyAggregator::LatencyAggregator(Options opts, QObject* parent)
    : QObject(parent), m_opts(std::move(opts)) {
    if (!m_opts.csvPath.isEmpty()) {
        QDir().mkpath(QFileInfo(m_opts.csvPath).absolutePath());
        m_csvFile.setFileName(m_opts.csvPath);
        if (!m_csvFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            qWarning() << "Failed to open CSV file" << m_opts.csvPath
                       << ":" << m_csvFile.errorString();
        } else {
            m_csvStream.setDevice(&m_csvFile);
            writeCsvHeaderIfNeeded();
        }
    }
}

LatencyAggregator::~LatencyAggregator() {
    if (m_csvFile.isOpen()) {
        m_csvStream.flush();
        m_csvFile.close();
    }
}

void LatencyAggregator::writeCsvHeaderIfNeeded() {
    if (m_csvHeaderWritten || !m_csvFile.isOpen()) return;
    // Schema version goes in a comment line so plain CSV readers ignore it.
    m_csvStream << "# version=1 component=03_latency_probe units=microseconds\n";
    m_csvStream << "ts_monotonic_us,frame_idx,src_label,"
                   "decode_queue_us,upload_paint_us,end_to_end_us,vsync\n";
    m_csvHeaderWritten = true;
}

void LatencyAggregator::pushAndTrim(Window& w, qint64 valueUs, qint64 nowUs) {
    w.samplesUs.push_back(valueUs);
    w.insertedAtUs.push_back(nowUs);
    const qint64 windowUs = qint64(m_opts.windowSeconds) * 1000000LL;
    while (!w.insertedAtUs.empty() && (nowUs - w.insertedAtUs.front()) > windowUs) {
        w.samplesUs.pop_front();
        w.insertedAtUs.pop_front();
    }
}

void LatencyAggregator::summarize(const std::deque<qint64>& samples,
                                  qint64& p50, qint64& p95, qint64& p99, qint64& maxv) {
    if (samples.empty()) {
        p50 = p95 = p99 = maxv = 0;
        return;
    }
    std::vector<qint64> sorted(samples.begin(), samples.end());
    std::sort(sorted.begin(), sorted.end());
    p50  = percentile(sorted, 0.50);
    p95  = percentile(sorted, 0.95);
    p99  = percentile(sorted, 0.99);
    maxv = sorted.back();
}

void LatencyAggregator::addSample(qint64 decodeQueueUs,
                                  qint64 uploadPaintUs,
                                  qint64 endToEndUs) {
    const qint64 nowUs = monotonicNowUs();
    pushAndTrim(m_decodeQueue, decodeQueueUs, nowUs);
    pushAndTrim(m_uploadPaint, uploadPaintUs, nowUs);
    pushAndTrim(m_endToEnd,    endToEndUs,    nowUs);

    ++m_totalSamples;
    m_lastEndToEndUs = endToEndUs;

    if (m_opts.csvEveryFrames > 0 && (m_totalSamples % m_opts.csvEveryFrames) == 0) {
        appendCsvRow(nowUs, decodeQueueUs, uploadPaintUs, endToEndUs);
    }
}

void LatencyAggregator::appendCsvRow(qint64 nowUs,
                                     qint64 decodeQueueUs,
                                     qint64 uploadPaintUs,
                                     qint64 endToEndUs) {
    if (!m_csvFile.isOpen()) return;
    m_csvStream
        << nowUs << ','
        << m_totalSamples << ','
        << m_opts.sourceLabel << ','
        << decodeQueueUs << ','
        << uploadPaintUs << ','
        << endToEndUs << ','
        << (m_opts.vsync ? "on" : "off")
        << '\n';
    if ((++m_csvRowsWritten % 32) == 0) {
        m_csvStream.flush();
    }
}

void LatencyAggregator::emitSummaryLine() {
    qint64 dqP50, dqP95, dqP99, dqMax;
    qint64 upP50, upP95, upP99, upMax;
    qint64 e2P50, e2P95, e2P99, e2Max;
    summarize(m_decodeQueue.samplesUs, dqP50, dqP95, dqP99, dqMax);
    summarize(m_uploadPaint.samplesUs, upP50, upP95, upP99, upMax);
    summarize(m_endToEnd.samplesUs,    e2P50, e2P95, e2P99, e2Max);

    // Sanity: if (dqP50 + upP50) drifts far from e2P50, the timestamping is
    // suspect. We don't fail the run on it (some natural skew exists due to
    // independent percentile picks), but we surface the delta inline so a
    // human reading the log can spot a regression in the contract itself.
    const qint64 sanityDeltaUs = (dqP50 + upP50) - e2P50;

    const int fps = int(m_decodeQueue.samplesUs.size()) / std::max(1, m_opts.windowSeconds);

    qInfo().noquote() << QString(
        "fps=%1 win=%2s n=%3 src=%4 vsync=%5 | "
        "decode_queue p50=%6 p95=%7 p99=%8 max=%9 ms | "
        "upload_paint p50=%10 p95=%11 p99=%12 max=%13 ms | "
        "end_to_end p50=%14 p95=%15 p99=%16 max=%17 ms | "
        "sanity(dq+up-e2)=%18 ms")
        .arg(fps).arg(m_opts.windowSeconds).arg(m_decodeQueue.samplesUs.size())
        .arg(m_opts.sourceLabel).arg(m_opts.vsync ? "on" : "off")
        .arg(formatMs(dqP50)).arg(formatMs(dqP95)).arg(formatMs(dqP99)).arg(formatMs(dqMax))
        .arg(formatMs(upP50)).arg(formatMs(upP95)).arg(formatMs(upP99)).arg(formatMs(upMax))
        .arg(formatMs(e2P50)).arg(formatMs(e2P95)).arg(formatMs(e2P99)).arg(formatMs(e2Max))
        .arg(formatMs(sanityDeltaUs));
}
