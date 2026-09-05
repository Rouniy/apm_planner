/*
 * speech_backend_probe - standalone diagnostic for the real QtAudioOutput
 * speech backend (Qt TextToSpeech through the installed engine plugin).
 *
 * This is intentionally NOT an automatic ctest: with --speak it produces sound.
 * Root wires the CMake target; suggested sources are this file,
 * src/audio/QtAudioOutput.cpp and src/audio/QueuedSpeechController.cpp linked
 * against Qt::Core plus ${APM_QT_AUDIO_LIBRARIES}.
 *
 * Usage:
 *   speech_backend_probe [--timeout-ms N] [--speak [PHRASE]] [--speak-timeout-ms N]
 *
 * Exit codes:
 *   0  backend reached Ready within the timeout (and, with --speak, the phrase
 *      was accepted and the queue returned to idle)
 *   2  built without Qt TextToSpeech (APM_HAS_QT_TEXT_TO_SPEECH undefined)
 *   3  no Qt TextToSpeech engine plugin available at runtime
 *   4  the engine reported an error (for example speech-dispatcher down)
 *   5  the backend never became ready within --timeout-ms
 *   6  --speak: the phrase was rejected by the queue
 *   7  --speak: the utterance did not complete within --speak-timeout-ms
 *   64 bad command line
 */

#include "audio/QtAudioOutput.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QTimer>

#include <functional>

namespace {

const int kDefaultReadyTimeoutMs = 5000;
const int kDefaultSpeakTimeoutMs = 30000;
const char *const kDefaultPhrase = "Проверка звука Mission Planner 10";

const char *stateName(SpeechBackendState state)
{
    switch (state) {
    case SpeechBackendState::NotCompiled:
        return "not-compiled";
    case SpeechBackendState::NoEngine:
        return "no-engine";
    case SpeechBackendState::RuntimeError:
        return "runtime-error";
    case SpeechBackendState::QueueUnavailable:
        return "queue-unavailable";
    case SpeechBackendState::Ready:
        return "ready";
    case SpeechBackendState::Busy:
        return "busy";
    }
    return "unknown";
}

void printDiagnostic(QTextStream &out, const char *label,
                     const SpeechBackendDiagnostic &diagnostic)
{
    out << label << ": state=" << stateName(diagnostic.state);
    if (!diagnostic.engine.isEmpty()) {
        out << " engine=" << diagnostic.engine;
    }
    if (!diagnostic.detail.isEmpty()) {
        out << " detail=\"" << diagnostic.detail << '"';
    }
    out << '\n';
    out.flush();
}

/** Pumps the event loop in 50 ms slices until the predicate holds or timeout. */
bool waitUntil(const std::function<bool()> &predicate, int timeoutMs)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate()) {
        if (elapsed.elapsed() >= timeoutMs) {
            return false;
        }
        QEventLoop slice;
        QTimer::singleShot(50, &slice, &QEventLoop::quit);
        slice.exec();
    }
    return true;
}

int usage(QTextStream &err)
{
    err << "usage: speech_backend_probe [--timeout-ms N] [--speak [PHRASE]] "
           "[--speak-timeout-ms N]\n";
    err.flush();
    return 64;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("APMPlanner"));
    QCoreApplication::setApplicationName(QStringLiteral("speech_backend_probe"));

    QTextStream out(stdout);
    QTextStream err(stderr);

    int readyTimeoutMs = kDefaultReadyTimeoutMs;
    int speakTimeoutMs = kDefaultSpeakTimeoutMs;
    bool speak = false;
    QString phrase = QString::fromUtf8(kDefaultPhrase);

    const QStringList arguments = QCoreApplication::arguments();
    for (int index = 1; index < arguments.size(); ++index) {
        const QString &argument = arguments.at(index);
        if (argument == QLatin1String("--help") || argument == QLatin1String("-h")) {
            usage(out);
            return 0;
        }
        if (argument == QLatin1String("--timeout-ms")
            || argument == QLatin1String("--speak-timeout-ms")) {
            if (index + 1 >= arguments.size()) {
                return usage(err);
            }
            bool ok = false;
            const int value = arguments.at(++index).toInt(&ok);
            if (!ok || value <= 0) {
                return usage(err);
            }
            if (argument == QLatin1String("--timeout-ms")) {
                readyTimeoutMs = value;
            } else {
                speakTimeoutMs = value;
            }
            continue;
        }
        if (argument == QLatin1String("--speak")) {
            speak = true;
            if (index + 1 < arguments.size()
                && !arguments.at(index + 1).startsWith(QLatin1String("--"))) {
                phrase = arguments.at(++index);
            }
            continue;
        }
        return usage(err);
    }

    out << "compiled-with-texttospeech: "
        << (QtAudioOutput::speechCompiledIn() ? "yes" : "no") << '\n';
    const QStringList engines = QtAudioOutput::availableSpeechEngines();
    out << "available-engines: "
        << (engines.isEmpty() ? QStringLiteral("(none)")
                              : engines.join(QStringLiteral(", ")))
        << '\n';
    out.flush();

    QtAudioOutput audio;
    SpeechBackendDiagnostic diagnostic = audio.speechBackendDiagnostic();
    printDiagnostic(out, "initial", diagnostic);

    if (diagnostic.state == SpeechBackendState::NotCompiled) {
        return 2;
    }
    if (diagnostic.state == SpeechBackendState::NoEngine) {
        return 3;
    }

    const bool ready = waitUntil([&audio, &diagnostic]() {
        diagnostic = audio.speechBackendDiagnostic();
        return diagnostic.accepting()
            || diagnostic.state == SpeechBackendState::RuntimeError;
    }, readyTimeoutMs);
    printDiagnostic(out, ready ? "settled" : "timeout", diagnostic);
    if (diagnostic.state == SpeechBackendState::RuntimeError) {
        return 4;
    }
    if (!ready) {
        return 5;
    }

    out << "voices: " << audio.availableVoices().size() << '\n';
    out.flush();

    if (!speak) {
        return 0;
    }

    if (!audio.speak(phrase)) {
        printDiagnostic(out, "speak-rejected", audio.speechBackendDiagnostic());
        return 6;
    }
    out << "speaking: \"" << phrase << "\"\n";
    out.flush();

    // The queue reports Busy while the engine speaks and returns to Ready when
    // the engine signals completion. A RuntimeError mid-utterance is a failure.
    const bool spoken = waitUntil([&audio, &diagnostic]() {
        diagnostic = audio.speechBackendDiagnostic();
        return diagnostic.state == SpeechBackendState::RuntimeError
            || (diagnostic.state == SpeechBackendState::Ready
                && audio.isSpeechIdle());
    }, speakTimeoutMs);
    printDiagnostic(out, spoken ? "completed" : "speak-timeout", diagnostic);
    if (diagnostic.state == SpeechBackendState::RuntimeError) {
        return 4;
    }
    return spoken ? 0 : 7;
}
