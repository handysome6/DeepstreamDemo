#pragma once

#include <QFile>
#include <QObject>
#include <QString>
#include <QTextStream>
#include <QtGlobal>

#include <deque>

// Receives one (decode_queue, upload_paint, end_to_end) sample per painted
// frame, computes rolling p50/p95/p99/max for each segment, and writes:
//   - one stdout summary line per second,
//   - one CSV row every csvEveryFrames samples.
//
// Lives entirely in the GUI thread; the GL widget calls addSample() and the
// QTimer-driven emitSummaryLine() — no locks needed.
//
// CSV column order is the contract advertised in the component README:
//   ts_monotonic_us, frame_idx, src_label, decode_queue_us, upload_paint_us,
//   end_to_end_us, vsync
//
// The first line of the file is a `# version=1 ...` comment so future
// downstream tooling can detect schema bumps.
class LatencyAggregator : public QObject {
    Q_OBJECT
public:
    struct Options {
        QString csvPath;          // empty -> no CSV file
        QString sourceLabel;      // copied verbatim into csv `src_label`
        bool    vsync = false;
        int     windowSeconds  = 5;
        int     csvEveryFrames = 30;
    };

    explicit LatencyAggregator(Options opts, QObject* parent = nullptr);
    ~LatencyAggregator() override;

    // Called once per painted frame, from the GUI thread.
    void addSample(qint64 decodeQueueUs,
                   qint64 uploadPaintUs,
                   qint64 endToEndUs);

    // Counters for the in-window status line.
    quint64 totalSamples()  const { return m_totalSamples; }
    qint64  lastEndToEndUs() const { return m_lastEndToEndUs; }

public slots:
    // Wired to a 1 Hz QTimer in main(); emits one stdout summary covering
    // the last `windowSeconds` worth of samples.
    void emitSummaryLine();

private:
    struct Window {
        std::deque<qint64> samplesUs;   // FIFO ordered by insertion = monotonic time
        std::deque<qint64> insertedAtUs;
    };

    void pushAndTrim(Window& w, qint64 valueUs, qint64 nowUs);
    static void summarize(const std::deque<qint64>& samples,
                          qint64& p50, qint64& p95, qint64& p99, qint64& maxv);
    void writeCsvHeaderIfNeeded();
    void appendCsvRow(qint64 nowUs,
                      qint64 decodeQueueUs,
                      qint64 uploadPaintUs,
                      qint64 endToEndUs);

    Options m_opts;

    Window m_decodeQueue;
    Window m_uploadPaint;
    Window m_endToEnd;

    quint64 m_totalSamples   = 0;
    quint64 m_csvRowsWritten = 0;
    qint64  m_lastEndToEndUs = 0;

    QFile       m_csvFile;
    QTextStream m_csvStream;
    bool        m_csvHeaderWritten = false;
};
