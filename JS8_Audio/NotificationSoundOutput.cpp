/**
 * @file NotificationSoundOutput.cpp
 * @brief Self-contained audio output for notification sounds.
 *
 * Designed for fire-and-forget playback of buffered audio data.
 * Manages its own buffer lifecycle and releases the audio device
 * automatically when playback completes.
 */

#include "NotificationSoundOutput.h"

#include <functional>

#include <QAudioSink>
#include <QBuffer>
#include <QDebug>
#include <QMetaObject>
#include <QThread>

NotificationSoundOutput::NotificationSoundOutput(QObject *parent)
    : QObject(parent)
{
    m_lifetimeTimer.start();
}

NotificationSoundOutput::~NotificationSoundOutput()
{
    if (m_sink) {
        disconnect(m_sink.get(), nullptr, this, nullptr);
        m_sink->stop();
    }
}

bool NotificationSoundOutput::ensureObjectThread(std::function<void()> fn)
{
    if (QThread::currentThread() == thread())
        return true;

    QMetaObject::invokeMethod(
        this,
        [fn = std::move(fn)]() mutable {
            fn();
        },
        Qt::QueuedConnection);

    return false;
}

void NotificationSoundOutput::logState(QString const &where) const
{
    qDebug().nospace()
        << "[NotificationSoundOutput] "
        << where
        << " t=" << m_lifetimeTimer.elapsed() << "ms"
        << " playSerial=" << m_playSerial
        << " sink=" << m_sink.get()
        << " buffer=" << m_buffer.get()
        << " creates=" << m_sinkCreateCount
        << " destroys=" << m_sinkDestroyCount
        << " thread=" << QThread::currentThread()
        << " objThread=" << thread();
}

/**
 * @brief Sets the audio device and buffer size.
 * @param device The QAudioDevice to use.
 * @param msBuffer The buffer size in milliseconds.
 */
void NotificationSoundOutput::setDevice(QAudioDevice const &device,
                                        unsigned const msBuffer)
{
    if (!ensureObjectThread([this, device, msBuffer]() {
            setDevice(device, msBuffer);
        })) {
        return;
        }

    m_device = device;
    m_msBuffer = msBuffer;
}

/**
 * @brief Sets the attenuation in dB.
 * @param a The attenuation value (0 = full volume).
 */
void NotificationSoundOutput::setAttenuation(qreal const a)
{
    if (!ensureObjectThread([this, a]() {
            setAttenuation(a);
        })) {
        return;
        }

    Q_ASSERT(0.0 <= a && a <= 999.0);
    m_volume = qPow(10.0, -a / 20.0);

    if (m_sink)
        m_sink->setVolume(m_volume);
}

/**
 * @brief Plays audio from the provided data and format.
 * @param data The raw PCM audio data.
 * @param format The QAudioFormat describing the data.
 *
 * Any currently playing notification is stopped and replaced.
 */
void NotificationSoundOutput::play(QByteArray const &data,
                                   QAudioFormat const &format)
{
    if (!ensureObjectThread([this, data, format]() {
            play(data, format);
        })) {
        return;
        }

    ++m_playSerial;
    logState("play ENTER");

    if (m_sink) {
        logState("play destroying existing sink");

        disconnect(m_sink.get(), nullptr, this, nullptr);

        qDebug().nospace()
            << "[NotificationSoundOutput] DESTROY sink="
            << m_sink.get()
            << " playSerial=" << m_playSerial;

        ++m_sinkDestroyCount;
        m_sink.reset();

        logState("play destroyed existing sink");
    }

    m_sink.reset(new QAudioSink(m_device, format));
    ++m_sinkCreateCount;

    qDebug().nospace()
        << "[NotificationSoundOutput] CREATE sink="
        << m_sink.get()
        << " playSerial=" << m_playSerial
        << " format=" << format;

    m_sink->setVolume(m_volume);
    m_currentFormat = format;

    if (m_msBuffer > 0) {
        m_sink->setBufferSize(
            m_sink->format().bytesForDuration(m_msBuffer));
    }

    if (m_buffer) {
        m_buffer->close();
        m_buffer.reset();
    }

    if (m_device.isNull()) {
        Q_EMIT error(tr("No audio output device configured."));
        return;
    }

    if (!format.isValid()) {
        Q_EMIT error(tr("Requested audio format is not valid."));
        return;
    }

    if (!m_device.isFormatSupported(format)) {
        Q_EMIT error(tr("Requested audio format is not supported on device."));
        return;
    }

    if (data.isEmpty()) {
        Q_EMIT error(tr("Audio data is empty."));
        return;
    }

    m_buffer.reset(new QBuffer);
    m_buffer->setData(data);

    if (!m_buffer->open(QIODevice::ReadOnly)) {
        m_buffer.reset();
        Q_EMIT error(tr("Unable to open audio buffer."));
        return;
    }

    // Only create a new sink if we don't have one, or if the
    // format has changed since the last playback.
    if (!m_sink || m_currentFormat != format) {
        m_sink.reset(new QAudioSink(m_device, format));
        m_sink->setVolume(m_volume);
        m_currentFormat = format;

        if (m_msBuffer > 0) {
            m_sink->setBufferSize(
                m_sink->format().bytesForDuration(m_msBuffer));
        }
    }

    connect(m_sink.get(), &QAudioSink::stateChanged,
            this, &NotificationSoundOutput::handleStateChanged);

    m_sink->start(m_buffer.get());

    logState("play AFTER start");

    if (m_sink->error() != QAudio::NoError) {
        Q_EMIT error(tr("Failed to start audio output."));
        m_sink->stop();
        if (m_buffer) {
            m_buffer->close();
            m_buffer.reset();
        }
    }
}

/**
 * @brief Stops playback and releases all resources.
 */
void NotificationSoundOutput::stop()
{
    if (!ensureObjectThread([this]() {
            stop();
        })) {
        return;
        }

    release();
}

// Private
void NotificationSoundOutput::release()
{
    logState("release ENTER");

    if (m_sink) {
        logState("release destroying sink");

        disconnect(m_sink.get(), nullptr, this, nullptr);

        qDebug().nospace()
            << "[NotificationSoundOutput] DESTROY sink="
            << m_sink.get()
            << " playSerial=" << m_playSerial;

        ++m_sinkDestroyCount;
        m_sink.reset();

        logState("release destroyed sink");
    }

    if (m_buffer) {
        logState("release closing buffer");
        m_buffer->close();
        m_buffer.reset();
        logState("release destroyed buffer");
    }

    logState("release EXIT");
}

/**
 * @brief Handles state changes from the QAudioSink.
 * @param newState The new audio state.
 */
void NotificationSoundOutput::handleStateChanged(QAudio::State const newState)
{
    auto *sink = qobject_cast<QAudioSink *>(sender());

    qDebug().nospace()
        << "[NotificationSoundOutput] stateChanged state="
        << newState
        << " sender=" << sink
        << " currentSink=" << m_sink.get()
        << " playSerial=" << m_playSerial
        << " thread=" << QThread::currentThread();

    switch (newState) {
    case QAudio::ActiveState:
        Q_EMIT status(tr("Active"));
        break;

    case QAudio::SuspendedState:
        Q_EMIT status(tr("Suspended"));
        break;

    case QAudio::IdleState:
    case QAudio::StoppedState:
        QMetaObject::invokeMethod(this, [this, sink]() {
            qDebug().nospace()
                << "[NotificationSoundOutput] queued idle cleanup sender="
                << sink
                << " currentSink=" << m_sink.get()
                << " playSerial=" << m_playSerial;

            if (!m_sink || m_sink.get() != sink) {
                qDebug()
                    << "[NotificationSoundOutput] queued idle cleanup ignored stale sink";
                return;
            }

            release();
            Q_EMIT status(tr("Idle"));
        }, Qt::QueuedConnection);
        break;
    }
}
