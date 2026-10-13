// xerum-host — load a plugin, inspect it, drive it, render audio to a WAV.
//
// Ticket 08. Console, no GUI, uses JUCE's headless plugin-hosting path
// (addHeadlessDefaultFormatsToManager) so no UI modules are pulled in.
//
// Examples:
//   xerum-host --plugin /Library/Audio/Plug-Ins/VST3/Serum2.vst3 --list-params
//   xerum-host --plugin .../Serum2.vst3 --list-programs
//   xerum-host --plugin .../Serum2.vst3 --out /tmp/a.wav --seconds 2 --note 60
//   xerum-host --plugin .../Serum2.vst3 --set "A Wavetable Pos=0.5" --program 3 --out /tmp/b.wav
//
// Deliberately NOT implemented here: applying a .SerumPreset file. Turning Serum's decoded
// parameter values into plugin parameters needs unit ranges and discrete-enum orders, which is
// ticket 14's contract, not the harness's job. --list-params/--list-programs exist to make that
// mapping discoverable from the real plugin rather than guessed.

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <iostream>

namespace
{

struct Args
{
    juce::String plugin;
    juce::String out;
    double seconds = 2.0;
    double sampleRate = 48000.0;
    int blockSize = 512;
    int note = 60;
    int program = -1;
    bool listParams = false;
    bool listPrograms = false;
    bool quiet = false;
    juce::StringArray sets; // "name=value", value interpreted as normalised 0..1
};

void printUsage()
{
    std::cout <<
        "usage: xerum-host --plugin <path> [options]\n"
        "\n"
        "  --plugin <path>        plugin bundle (VST3/AU) to load          [required]\n"
        "  --list-params          print the plugin's parameter surface, then exit\n"
        "  --list-programs        print the plugin's program list, then exit\n"
        "  --set \"name=value\"     set a parameter by name (value is normalised 0..1)\n"
        "  --program <n>          select program n before rendering\n"
        "  --out <file.wav>       render this many seconds to a WAV\n"
        "  --seconds <n>          render length, default 2\n"
        "  --sr <n>               sample rate, default 48000\n"
        "  --block <n>            block size, default 512\n"
        "  --note <n>             MIDI note to play, default 60\n"
        "  --quiet                suppress the render summary\n";
}

bool parseArgs (Args& a, const juce::StringArray& argv)
{
    for (int i = 0; i < argv.size(); ++i)
    {
        const auto arg = argv[i];
        auto next = [&]() -> juce::String { return ++i < argv.size() ? argv[i] : juce::String(); };

        if (arg == "--plugin")            a.plugin = next();
        else if (arg == "--out")          a.out = next();
        else if (arg == "--seconds")      a.seconds = next().getDoubleValue();
        else if (arg == "--sr")           a.sampleRate = next().getDoubleValue();
        else if (arg == "--block")        a.blockSize = next().getIntValue();
        else if (arg == "--note")         a.note = next().getIntValue();
        else if (arg == "--program")      a.program = next().getIntValue();
        else if (arg == "--set")          a.sets.add (next());
        else if (arg == "--list-params")  a.listParams = true;
        else if (arg == "--list-programs")a.listPrograms = true;
        else if (arg == "--quiet")        a.quiet = true;
        else if (arg == "--help" || arg == "-h") { printUsage(); return false; }
        else
        {
            std::cerr << "unrecognised argument: " << arg << "\n";
            return false;
        }
    }

    if (a.plugin.isEmpty())
    {
        std::cerr << "--plugin is required\n";
        return false;
    }
    return true;
}

juce::File resolveFile (const juce::String& path)
{
    juce::File f (path);
    if (f.exists())
        return f;
    return juce::File::getCurrentWorkingDirectory().getChildFile (path);
}

juce::String parameterName (const juce::AudioProcessorParameter& p)
{
    return p.getName (256);
}

} // namespace

int main (int argc, char* argv[])
{
    // Hosted plugins (VST3 especially) expect a message manager to exist even when they have no UI.
    juce::ScopedJuceInitialiser_GUI juceInit;

    Args args;
    if (! parseArgs (args, juce::StringArray (argv + 1, argc - 1)))
        return 2;

    const auto file = resolveFile (args.plugin);

    if (! file.exists())
    {
        std::cerr << "plugin not found: " << file.getFullPathName() << "\n";
        return 1;
    }

    juce::AudioPluginFormatManager manager;
    juce::addHeadlessDefaultFormatsToManager (manager);

    juce::AudioPluginFormat* format = nullptr;
    juce::OwnedArray<juce::PluginDescription> descriptions;

    for (auto* f : manager.getFormats())
    {
        if (! f->fileMightContainThisPluginType (file.getFullPathName()))
            continue;

        f->findAllTypesForFile (descriptions, file.getFullPathName());

        if (! descriptions.isEmpty())
        {
            format = f;
            break;
        }
    }

    if (format == nullptr || descriptions.isEmpty())
    {
        std::cerr << "no plugin description found in " << file.getFullPathName() << "\n";
        return 1;
    }

    for (auto* d : descriptions)
        std::cout << "found: " << d->name << " | " << d->pluginFormatName
                  << " | instrument=" << (d->isInstrument ? "yes" : "no")
                  << " | " << juce::String (d->numInputChannels) << " in, "
                  << juce::String (d->numOutputChannels) << " out\n";

    const auto& description = *descriptions.getFirst();

    juce::String error;
    auto instance = manager.createPluginInstance (description, args.sampleRate, args.blockSize, error);

    if (instance == nullptr)
    {
        std::cerr << "could not create instance: " << error << "\n";
        return 1;
    }

    instance->enableAllBuses();
    instance->prepareToPlay (args.sampleRate, args.blockSize);

    if (args.listPrograms)
    {
        std::cout << "\nprograms: " << instance->getNumPrograms() << "\n";
        for (int i = 0; i < instance->getNumPrograms(); ++i)
            std::cout << "  [" << i << "] " << instance->getProgramName (i) << "\n";
    }

    if (args.listParams)
    {
        const auto& params = instance->getParameters();
        std::cout << "\nparameters: " << params.size() << "\n";
        for (int i = 0; i < params.size(); ++i)
        {
            auto* p = params[i];
            if (p == nullptr)
                continue;
            std::cout << "  [" << i << "] " << parameterName (*p)
                      << "  value=" << juce::String (p->getValue(), 4)
                      << "  text=\"" << p->getText (p->getValue(), 64) << "\""
                      << "  steps=" << p->getNumSteps()
                      << (p->isDiscrete() ? " discrete" : "")
                      << "\n";
        }
    }

    if (args.program >= 0)
    {
        if (args.program < instance->getNumPrograms())
        {
            instance->setCurrentProgram (args.program);
            std::cout << "\nselected program " << args.program << ": "
                      << instance->getProgramName (args.program) << "\n";
        }
        else
        {
            std::cerr << "program " << args.program << " out of range\n";
            return 1;
        }
    }

    if (! args.sets.isEmpty())
    {
        const auto& params = instance->getParameters();
        for (const auto& spec : args.sets)
        {
            const auto name = spec.upToFirstOccurrenceOf ("=", false, false).trim();
            const auto value = spec.fromFirstOccurrenceOf ("=", false, false).trim().getFloatValue();

            bool matched = false;
            for (auto* p : params)
            {
                if (p != nullptr && parameterName (*p).equalsIgnoreCase (name))
                {
                    p->setValueNotifyingHost (value);
                    std::cout << "set \"" << name << "\" = " << value << "\n";
                    matched = true;
                    break;
                }
            }
            if (! matched)
                std::cerr << "no parameter named \"" << name << "\"\n";
        }
    }

    if (args.listParams || args.listPrograms)
        return 0;

    if (args.out.isEmpty())
    {
        std::cerr << "--out is required to render (or use --list-params/--list-programs)\n";
        return 2;
    }

    const auto outFile = juce::File::createFileWithoutCheckingPath (args.out);
    outFile.deleteFile();

    const int numChannels = juce::jmax (1, juce::jmin (2, instance->getTotalNumOutputChannels()));
    const auto totalSamples = (juce::int64) (args.seconds * args.sampleRate);
    const auto blockSize = juce::jmax (1, args.blockSize);

    juce::AudioBuffer<float> buffer (numChannels, blockSize);
    juce::MidiBuffer midi;
    const bool isInstrument = description.isInstrument;

    // Instrument: note on, then note off at halfway. Effect: feed a short impulse so there is
    // something to process.
    const auto noteOnSamples  = (juce::int64) (0.02 * args.sampleRate);
    const auto noteOffSamples = (juce::int64) (0.5 * args.sampleRate);

    double peak = 0.0;
    juce::int64 written = 0;

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = outFile.createOutputStream();

    if (stream == nullptr)
    {
        std::cerr << "could not create output stream\n";
        return 1;
    }

    // JUCE 9 replaced the long createWriterFor parameter list with a builder-style options object.
    // 32-bit float WAV keeps the reference renders bit-comparable rather than requantised.
    const auto writerOptions = juce::AudioFormatWriterOptions()
                                   .withSampleRate (args.sampleRate)
                                   .withNumChannels (numChannels)
                                   .withBitsPerSample (32)
                                   .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);

    auto writer = wav.createWriterFor (stream, writerOptions);

    if (writer == nullptr)
    {
        std::cerr << "could not create WAV writer\n";
        return 1;
    }

    while (written < totalSamples)
    {
        const auto thisBlock = (int) juce::jmin ((juce::int64) blockSize, totalSamples - written);

        buffer.clear();
        midi.clear();

        if (written <= noteOnSamples && noteOnSamples < written + thisBlock)
            midi.addEvent (juce::MidiMessage::noteOn (1, args.note, (juce::uint8) 100),
                           (int) (noteOnSamples - written));

        if (written <= noteOffSamples && noteOffSamples < written + thisBlock)
            midi.addEvent (juce::MidiMessage::noteOff (1, args.note),
                           (int) (noteOffSamples - written));

        if (! isInstrument && thisBlock > 0)
            buffer.setSample (0, 0, 1.0f);

        instance->processBlock (buffer, midi);

        for (int ch = 0; ch < numChannels; ++ch)
            peak = juce::jmax (peak, (double) buffer.getMagnitude (ch, 0, thisBlock));

        if (! writer->writeFromAudioSampleBuffer (buffer, 0, thisBlock))
        {
            std::cerr << "write failed at sample " << written << "\n";
            return 1;
        }

        written += thisBlock;
    }

    writer.reset();

    if (! args.quiet)
    {
        std::cout << "\nrendered " << args.seconds << "s @ " << args.sampleRate << "Hz, block "
                  << blockSize << ", note " << args.note << " -> " << outFile.getFullPathName() << "\n"
                  << "channels=" << numChannels << "  peak amplitude=" << juce::String (peak, 6)
                  << (peak <= 0.0 ? "   *** SILENT ***" : "") << "\n";
    }

    return 0;
}
