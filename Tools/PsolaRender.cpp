// ModernPitch - Copyright (C) 2026 Mickaël Doux (Soft Studio)
// SPDX-License-Identifier: AGPL-3.0-or-later

// Offline test bench for the ModernPitch PSOLA engine.
//
//   PsolaRender synth
//       Synthetic plucked strings (49-330 Hz) shifted by -12..+12 st: pitch
//       error, output periodicity, attack delay and mean engine lag.
//
//   PsolaRender render <input.wav> <outputDir> [semitones ...] [options]
//       Renders the file with the plugin's settings for each shift (default
//       -12 -5 +7) and prints lag, pitched share and speed.
//       --legacy      YIN note tracking (setImprovedTracking(false))
//       --no-restart  keep the old grain train on a new note
//       --tight       shorter pitched grains
//       --events      also write <name>.events.csv (detector, marks, grains)
//       --ring        write the RING voice (wet) instead of the shifted signal

#include <JuceHeader.h>
#include "../Source/PsolaEngine.h"
#include "../Source/RingModulator.h"
#include <algorithm>
#include <chrono>
#include <vector>

namespace
{
constexpr int blockSize = 256;

struct Settings
{
    bool improvedTracking = true;
    bool restartOnNewNote = true;
    bool tightGrains = false;
    bool ring = false;
};

struct RenderStats
{
    double averageLagMs = 0.0;
    double realtimeFactor = 0.0;
    double pitchedShare = 0.0;
};

RenderStats render(PsolaEngine& engine, const Settings& settings, float semitones,
                   const juce::AudioBuffer<float>& input, juce::AudioBuffer<float>& output,
                   double sampleRate)
{
    engine.setImprovedTracking(settings.improvedTracking);
    engine.setRestartOnNewNote(settings.restartOnNewNote);
    engine.setTightGrains(settings.tightGrains);
    engine.prepare(sampleRate);
    engine.setPitchSemitones(0, semitones);
    engine.setPitchSemitones(1, semitones - 12.0f);

    const int channels = input.getNumChannels();
    const int length = input.getNumSamples();
    output.setSize(channels, length);
    juce::AudioBuffer<float> in(channels, blockSize), out(channels, blockSize), second(channels, blockSize);
    std::array<float, blockSize> periods {};
    RingModulator ring;
    ring.prepare(sampleRate);
    // As in the plugin: at 0 st the unshifted input is modulated, without delay.
    const bool shifting = std::abs(semitones) >= 0.0001f;

    double lagSum = 0.0;
    int lagBlocks = 0;
    const auto started = std::chrono::steady_clock::now();
    for (int start = 0; start < length; start += blockSize)
    {
        const int n = juce::jmin(blockSize, length - start);
        in.setSize(channels, n, false, false, true);
        out.setSize(channels, n, false, false, true);
        second.setSize(channels, n, false, false, true);
        for (int ch = 0; ch < channels; ++ch)
            in.copyFrom(ch, 0, input, ch, start, n);
        engine.process(in, out, second, periods.data());
        if (settings.ring)
        {
            const double lag = shifting ? engine.getLagSamples(0) : 0.0;
            const double ratio = shifting ? engine.getCurrentRatio(0) : 1.0;
            const auto& modulated = shifting ? out : in;
            for (int i = 0; i < n; ++i)
            {
                const float carrier = ring.next(periods[(size_t) i], lag, ratio);
                for (int ch = 0; ch < channels; ++ch)
                    out.setSample(ch, i, modulated.getSample(ch, i) * carrier);
            }
        }
        for (int ch = 0; ch < channels; ++ch)
            output.copyFrom(ch, start, out, ch, 0, n);
        lagSum += engine.getLagSamples(0);
        ++lagBlocks;
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    RenderStats stats;
    stats.averageLagMs = 1000.0 * lagSum / juce::jmax(1, lagBlocks) / sampleRate;
    stats.realtimeFactor = elapsed > 0.0 ? ((double) length / sampleRate) / elapsed : 0.0;
    const auto& kinds = engine.getGrainKindSamples(0);
    stats.pitchedShare = (double) kinds[0] / (double) juce::jmax<int64_t>(1, kinds[0] + kinds[1]);
    return stats;
}

// Karplus-Strong pluck with a DI-like bright attack and no DC offset.
juce::AudioBuffer<float> makePluck(double sampleRate, double frequency, double seconds, double onset)
{
    const int length = (int) (seconds * sampleRate);
    juce::AudioBuffer<float> buffer(1, length);
    buffer.clear();
    // Loop delay = integer line + averaging filter (0.5) + allpass fraction.
    const double exactDelay = sampleRate / frequency - 0.5;
    const int integerDelay = (int) std::floor(exactDelay);
    const double fraction = exactDelay - integerDelay;
    std::vector<float> line((size_t) integerDelay);
    juce::Random random(1234);
    double mean = 0.0;
    for (auto& value : line)
    {
        value = random.nextFloat() * 2.0f - 1.0f;
        mean += value;
    }
    mean /= (double) line.size();
    for (auto& value : line)
        value -= (float) mean;

    const float allpass = (float) ((1.0 - fraction) / (1.0 + fraction));
    float allpassState = 0.0f, allpassInput = 0.0f;
    int index = 0;
    for (int i = (int) (onset * sampleRate); i < length; ++i)
    {
        const int next = (index + 1) % integerDelay;
        const float averaged = 0.996f * 0.5f * (line[(size_t) index] + line[(size_t) next]);
        const float delayed = allpass * (averaged - allpassState) + allpassInput;
        allpassInput = averaged;
        allpassState = delayed;
        buffer.setSample(0, i, 0.5f * line[(size_t) index]);
        line[(size_t) index] = delayed;
        index = next;
    }
    return buffer;
}

struct PitchMeasure
{
    double frequency = 0.0;
    double aperiodicity = 1.0; // YIN CMND at the selected lag: 0 = perfectly periodic
};

// Brute-force full-rate YIN over long windows; median of several frames.
PitchMeasure measurePitch(const juce::AudioBuffer<float>& buffer, double sampleRate, int from, int to)
{
    const float* x = buffer.getReadPointer(0);
    const int maxLag = (int) (sampleRate / 20.0);
    const int window = 4096;
    std::vector<double> frequencies, aperiodicities;
    std::vector<double> cmnd((size_t) maxLag + 2, 1.0);
    for (int start = from; start + window + maxLag < to; start += window + window / 2)
    {
        double running = 0.0;
        int selected = -1;
        for (int tau = 1; tau <= maxLag + 1; ++tau)
        {
            double sum = 0.0;
            for (int j = 0; j < window; ++j)
            {
                const double delta = (double) x[start + j] - x[start + j + tau];
                sum += delta * delta;
            }
            running += sum;
            cmnd[(size_t) tau] = running > 0.0 ? sum * tau / running : 1.0;
        }
        for (int tau = (int) (sampleRate / 2000.0); tau <= maxLag; ++tau)
            if (cmnd[(size_t) tau] < 0.2)
            {
                while (tau < maxLag && cmnd[(size_t) (tau + 1)] < cmnd[(size_t) tau])
                    ++tau;
                selected = tau;
                break;
            }
        if (selected < 0)
            continue;
        const double l = cmnd[(size_t) (selected - 1)], c = cmnd[(size_t) selected],
                     r = cmnd[(size_t) (selected + 1)];
        const double den = l - 2.0 * c + r;
        const double lag = selected + (std::abs(den) > 1e-12 ? 0.5 * (l - r) / den : 0.0);
        frequencies.push_back(sampleRate / lag);
        aperiodicities.push_back(c);
    }
    PitchMeasure result;
    if (frequencies.empty())
        return result;
    std::sort(frequencies.begin(), frequencies.end());
    std::sort(aperiodicities.begin(), aperiodicities.end());
    result.frequency = frequencies[frequencies.size() / 2];
    result.aperiodicity = aperiodicities[aperiodicities.size() / 2];
    return result;
}

// First sample whose 1 ms RMS reaches 10 % of the maximum 1 ms RMS.
int findAttack(const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    const float* x = buffer.getReadPointer(0);
    const int n = buffer.getNumSamples();
    const int span = juce::jmax(1, (int) (0.001 * sampleRate));
    std::vector<double> rms((size_t) n, 0.0);
    double sum = 0.0, peak = 0.0;
    for (int i = 0; i < n; ++i)
    {
        sum += (double) x[i] * x[i];
        if (i >= span)
            sum -= (double) x[i - span] * x[i - span];
        rms[(size_t) i] = std::sqrt(juce::jmax(0.0, sum) / span);
        peak = juce::jmax(peak, rms[(size_t) i]);
    }
    for (int i = 0; i < n; ++i)
        if (rms[(size_t) i] >= 0.1 * peak)
            return i;
    return -1;
}

bool writeWav(const juce::File& file, const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    file.deleteFile();
    juce::WavAudioFormat format;
    std::unique_ptr<juce::FileOutputStream> stream(file.createOutputStream());
    if (stream == nullptr)
        return false;
    std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(
        stream.get(), sampleRate, (unsigned int) buffer.getNumChannels(), 24, {}, 0));
    if (writer == nullptr)
        return false;
    stream.release();
    return writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
}

int runSyntheticTest()
{
    constexpr double sampleRate = 48000.0;
    constexpr double onset = 0.2;
    const double frequencies[] = { 49.0, 55.0, 82.41, 110.0, 196.0, 329.63 };
    const float shifts[] = { -12.0f, -5.0f, -2.0f, 7.0f, 12.0f };
    const int from = (int) (0.6 * sampleRate), to = (int) (1.5 * sampleRate);
    auto engine = std::make_unique<PsolaEngine>();

    std::printf("Synthetic pluck test (48 kHz)\n"
                "err = cents vs shifted input pitch | ap = output aperiodicity x100 (lower is cleaner)\n"
                "atk = attack delay ms | lag = mean engine lag ms\n\n");
    std::printf("%5s", "st");
    for (const double f : frequencies)
        std::printf(" | %6.1fHz  err ap atk  lag", f);
    std::printf("\n");

    std::vector<juce::AudioBuffer<float>> inputs;
    std::vector<PitchMeasure> references;
    for (const double frequency : frequencies)
    {
        inputs.push_back(makePluck(sampleRate, frequency, 1.6, onset));
        references.push_back(measurePitch(inputs.back(), sampleRate, from, to));
    }

    juce::AudioBuffer<float> output;
    for (const float shift : shifts)
    {
        std::printf("%+5.0f", shift);
        for (size_t f = 0; f < inputs.size(); ++f)
        {
            const auto stats = render(*engine, {}, shift, inputs[f], output, sampleRate);
            const double expected = references[f].frequency * std::pow(2.0, shift / 12.0);
            const auto measured = measurePitch(output, sampleRate, from, to);
            const double cents = measured.frequency > 0.0
                ? 1200.0 * std::log2(measured.frequency / expected) : 9999.0;
            const double attackMs = 1000.0 * (findAttack(output, sampleRate)
                                              - findAttack(inputs[f], sampleRate)) / sampleRate;
            std::printf(" | %+11.1f %2.0f %3.0f %4.1f", cents, 100.0 * measured.aperiodicity,
                        attackMs, stats.averageLagMs);
        }
        std::printf("\n");
    }
    return 0;
}

void writeEvents(const juce::File& file, const std::vector<PsolaEngine::Event>& events, double sampleRate)
{
    const char* const names[] = { "estimate", "voiced_on", "voiced_off", "reset_jump",
                                  "reset_correlation", "new_note", "mark", "grain" };
    juce::String csv = "time,type,value,extra\n";
    for (const auto& e : events)
        csv << juce::String(e.position / sampleRate, 5) << "," << names[(int) e.type] << ","
            << juce::String(e.value, 4) << "," << juce::String(e.extra, 4) << "\n";
    file.replaceWithText(csv);
}

int runRender(int argc, char* argv[])
{
    const auto cwd = juce::File::getCurrentWorkingDirectory();
    const juce::File inputFile = cwd.getChildFile(argv[2]);
    const juce::File outputDirectory = cwd.getChildFile(argv[3]);
    outputDirectory.createDirectory();

    Settings settings;
    bool events = false;
    std::vector<float> shifts;
    for (int i = 4; i < argc; ++i)
    {
        const juce::String arg(argv[i]);
        if (arg == "--legacy")          settings.improvedTracking = false;
        else if (arg == "--no-restart") settings.restartOnNewNote = false;
        else if (arg == "--tight")      settings.tightGrains = true;
        else if (arg == "--events")     events = true;
        else if (arg == "--ring")       settings.ring = true;
        else                            shifts.push_back((float) arg.getDoubleValue());
    }
    if (shifts.empty())
        shifts = { -12.0f, -5.0f, 7.0f };

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(inputFile));
    if (reader == nullptr)
    {
        std::printf("cannot read %s\n", inputFile.getFullPathName().toRawUTF8());
        return 1;
    }
    const int channels = (int) juce::jmin(2u, reader->numChannels);
    juce::AudioBuffer<float> input(channels, (int) reader->lengthInSamples);
    reader->read(&input, 0, (int) reader->lengthInSamples, 0, true, channels > 1);
    const double sampleRate = reader->sampleRate;

    std::printf("%s (%.0f Hz, %.1f s)\n", inputFile.getFileName().toRawUTF8(), sampleRate,
                input.getNumSamples() / sampleRate);
    std::printf("%6s %8s %9s %6s\n", "st", "lag ms", "pitched%", "xRT");
    auto engine = std::make_unique<PsolaEngine>();
    juce::AudioBuffer<float> output;
    for (const float shift : shifts)
    {
        std::vector<PsolaEngine::Event> log;
        engine->setEventLog(events ? &log : nullptr);
        const auto stats = render(*engine, settings, shift, input, output, sampleRate);
        engine->setEventLog(nullptr);
        std::printf("%+6.0f %8.1f %9.1f %6.0f\n", shift, stats.averageLagMs, 100.0 * stats.pitchedShare,
                    stats.realtimeFactor);

        const auto name = inputFile.getFileNameWithoutExtension() + " " + (shift > 0 ? "+" : "")
                        + juce::String((int) shift);
        writeWav(outputDirectory.getChildFile(name + ".wav"), output, sampleRate);
        if (events)
            writeEvents(outputDirectory.getChildFile(name + ".events.csv"), log, sampleRate);
    }
    return 0;
}
} // namespace

int main(int argc, char* argv[])
{
    const juce::String command = argc > 1 ? juce::String(argv[1]) : juce::String();
    if (command == "synth")
        return runSyntheticTest();
    if (command == "render" && argc >= 4)
        return runRender(argc, argv);

    std::printf("usage:\n"
                "  PsolaRender synth\n"
                "  PsolaRender render <input.wav> <outputDir> [semitones ...]"
                " [--legacy] [--no-restart] [--tight] [--events] [--ring]\n");
    return 1;
}
