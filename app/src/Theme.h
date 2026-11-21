// Visual identity loaded from design/tokens.json (embedded as BinaryData), so
// every platform build uses exactly the same colors, sizes and breakpoints.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "BinaryData.h"
#include "ynot/json.hpp"

namespace ynotapp {

struct Theme {
    juce::Colour background, surface, surfaceRaised, border, text, textMuted, record, primary, secondary, accent;
    float bodySize = 13, titleSize = 17;

    static juce::Colour hex(const std::string& s) {
        return juce::Colour::fromString("ff" + juce::String(s).trimCharactersAtStart("#"));
    }

    static Theme load(bool dark = true) {
        auto tokens = ynot::Json::parse(std::string(BinaryData::tokens_json, BinaryData::tokens_jsonSize));
        const auto& color = tokens.at("color");
        const auto& mode = color.at(dark ? "dark" : "light");
        const auto& brand = color.at("brand");
        Theme t;
        t.background = hex(mode.getString("background", "#000000"));
        t.surface = hex(mode.getString("surface", "#000000"));
        t.surfaceRaised = hex(mode.getString("surfaceRaised", "#000000"));
        t.border = hex(mode.getString("border", "#444444"));
        t.text = hex(mode.getString("text", "#ffffff"));
        t.textMuted = hex(mode.getString("textMuted", "#aaaaaa"));
        t.record = hex(mode.getString("record", "#ff0000"));
        t.primary = hex(brand.getString("primary", "#7C5CFF"));
        t.secondary = hex(brand.getString("secondary", "#2BD9A8"));
        t.accent = hex(brand.getString("accent", "#FFB547"));
        const auto& size = tokens.at("typography").at("size");
        t.bodySize = static_cast<float>(size.getNumber("body", 13));
        t.titleSize = static_cast<float>(size.getNumber("title", 17));
        return t;
    }

    void apply(juce::LookAndFeel_V4& lf) const {
        lf.setColour(juce::ResizableWindow::backgroundColourId, background);
        lf.setColour(juce::TextButton::buttonColourId, surfaceRaised);
        lf.setColour(juce::TextButton::buttonOnColourId, primary);
        lf.setColour(juce::TextButton::textColourOffId, text);
        lf.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        lf.setColour(juce::ComboBox::backgroundColourId, surfaceRaised);
        lf.setColour(juce::ComboBox::textColourId, text);
        lf.setColour(juce::ComboBox::outlineColourId, border);
        lf.setColour(juce::PopupMenu::backgroundColourId, surfaceRaised);
        lf.setColour(juce::PopupMenu::textColourId, text);
        lf.setColour(juce::TextEditor::backgroundColourId, surface);
        lf.setColour(juce::TextEditor::textColourId, text);
        lf.setColour(juce::TextEditor::outlineColourId, border);
        lf.setColour(juce::Label::textColourId, text);
        lf.setColour(juce::ListBox::backgroundColourId, surface);
    }
};

}  // namespace ynotapp
