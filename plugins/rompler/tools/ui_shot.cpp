#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cstdlib>
#include <iostream>

/**
    Headless UI snapshot tool.

    Creates the real RomplerEditor without a host window, gives it a few timer
    ticks so the peak meter and any async state settle, then renders the
    component into an Image and writes it as a PNG.

    Usage:
        ui_shot <output.png> [width height focus-order]
*/
int main (int argc, char* argv[])
{
    // argv[0] is the program name, so N user arguments arrive as argc == N+1.
    if (argc != 2 && argc != 4 && argc != 5)
    {
        std::cerr << "usage: ui_shot <output.png> [width height [focus-order]]\n";
        return 2;
    }

    const juce::ScopedJuceInitialiser_GUI juceInitialiser;

    aod::RomplerProcessor processor;
    processor.setPlayConfigDetails (0, 2, 48000.0, 512);
    processor.prepareToPlay (48000.0, 512);

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    if (editor == nullptr)
    {
        std::cerr << "createEditor returned null\n";
        return 1;
    }

    if (argc == 4 || argc == 5)
    {
        const int width = juce::String (argv[2]).getIntValue();
        const int height = juce::String (argv[3]).getIntValue();
        if (width <= 0 || height <= 0)
        {
            std::cerr << "width and height must be positive\n";
            return 2;
        }
        editor->setSize (width, height);
    }

    // Optional focus target: ui_shot <out.png> [w h] [focus-order-index].
    // Focus has to be granted after the layout settles, and only onto a
    // component that is showing inside a live peer.
    if (argc == 5)
    {
        editor->addToDesktop (juce::ComponentPeer::windowIsTemporary);
        editor->setVisible (true);

        const int order = juce::String (argv[4]).getIntValue();
        const auto traversable = editor->createFocusTraverser();
        if (traversable != nullptr)
            for (auto* c = traversable->getDefaultComponent (editor.get()); c != nullptr;
                 c = traversable->getNextComponent (c))
                if (c->getWantsKeyboardFocus() && c->getExplicitFocusOrder() == order)
                {
                    c->grabKeyboardFocus();
                    if (! c->hasKeyboardFocus (true))
                        std::cerr << "warning: focus order " << order << " did not take\n";
                }
    }

    // Let the editor's Timer run a handful of ticks so meter state / labels
    // have a chance to draw in their settled form.
    for (int i = 0; i < 8; ++i)
        juce::Thread::sleep (50);

    const auto bounds = editor->getBounds();
    const int w = juce::jmax (1, bounds.getWidth());
    const int h = juce::jmax (1, bounds.getHeight());

    juce::Image snapshot (juce::Image::ARGB, w, h, true);
    juce::Graphics g (snapshot);
    g.setColour (juce::Colours::black);
    g.fillAll();
    editor->paintEntireComponent (g, false);

    const juce::File outFile (juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]));
    juce::PNGImageFormat png;
    juce::FileOutputStream stream (outFile);
    if (! stream.openedOk())
    {
        std::cerr << "cannot open output: " << outFile.getFullPathName() << "\n";
        return 1;
    }

    if (! png.writeImageToStream (snapshot, stream))
    {
        std::cerr << "PNG write failed\n";
        return 1;
    }

    std::cout << "wrote " << outFile.getFullPathName()
              << " (" << w << "x" << h << ")\n";
    return 0;
}
