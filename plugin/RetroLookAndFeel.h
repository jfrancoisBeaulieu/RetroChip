// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SleeperMusic

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace retrochip
{
namespace palette
{
inline const juce::Colour background { 0xff1f1e24 };
inline const juce::Colour panel { 0xff2d2c34 };
inline const juce::Colour panelEdge { 0xff46454f };
inline const juce::Colour valueBoxEdge { 0xff8e989b }; // the frame of a knob's value, JUCE's own grey
inline const juce::Colour text { 0xffece6d6 };
inline const juce::Colour dimText { 0xffa6a196 };
inline const juce::Colour accent { 0xffd8402f };
inline const juce::Colour accentText { 0xffffffff };
} // namespace palette

class RetroLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    RetroLookAndFeel()
    {
        using namespace palette;
        setColour(juce::ResizableWindow::backgroundColourId, background);
        setColour(juce::Label::textColourId, text);
        setColour(juce::TextButton::buttonColourId, panel);
        setColour(juce::TextButton::buttonOnColourId, accent);
        setColour(juce::TextButton::textColourOffId, text);
        setColour(juce::TextButton::textColourOnId, accentText);
        setColour(juce::ComboBox::backgroundColourId, panel);
        setColour(juce::ComboBox::textColourId, text);
        setColour(juce::ComboBox::outlineColourId, panelEdge);
        setColour(juce::ComboBox::arrowColourId, text);
        setColour(juce::PopupMenu::backgroundColourId, panel);
        setColour(juce::PopupMenu::textColourId, text);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, accent);
        setColour(juce::PopupMenu::highlightedTextColourId, accentText);
        setColour(juce::Slider::textBoxTextColourId, text);
        // Boxes made before this look and feel was set have JUCE's frame, so the ones made after get the same.
        setColour(juce::Slider::textBoxOutlineColourId, valueBoxEdge);
        setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour(juce::ToggleButton::textColourId, text);
        setColour(juce::ToggleButton::tickColourId, accent);
        setColour(juce::ToggleButton::tickDisabledColourId, dimText);
    }

    static juce::Font retroFont(float height, bool bold = false)
    {
        return juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), height,
                                            bold ? juce::Font::bold : juce::Font::plain));
    }

    // A compact knob, or the text box of one, is set in a smaller type to fit a narrow column.
    static bool isCompact(const juce::Component* component)
    {
        return component != nullptr && static_cast<bool>(component->getProperties()["compact"]);
    }

    juce::Font getLabelFont(juce::Label& label) override
    {
        return retroFont(isCompact(&label) || isCompact(label.getParentComponent()) ? 11.0f : 13.0f);
    }

    juce::BorderSize<int> getLabelBorderSize(juce::Label& label) override
    {
        return isCompact(label.getParentComponent()) ? juce::BorderSize<int>(0, 1, 0, 1) : label.getBorderSize();
    }

    juce::Font getComboBoxFont(juce::ComboBox&) override { return retroFont(14.0f); }
    juce::Font getPopupMenuFont() override { return retroFont(14.0f); }
    juce::Font getTextButtonFont(juce::TextButton& button, int) override
    {
        return retroFont(isCompact(&button) ? 10.0f : 13.0f, true); // compact: the language switch under the keyboard
    }

    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& button, bool isHighlighted, bool isDown) override
    {
        constexpr float boxSize = 16.0f;
        const float boxTop = (static_cast<float>(button.getHeight()) - boxSize) / 2.0f;
        drawTickBox(g, button, 2.0f, boxTop, boxSize, boxSize, button.getToggleState(), button.isEnabled(), isHighlighted, isDown);

        g.setColour(button.findColour(juce::ToggleButton::textColourId));
        g.setFont(retroFont(13.0f));
        g.drawText(button.getButtonText(), button.getLocalBounds().withTrimmedLeft(24), juce::Justification::centredLeft);
    }

    void drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                              bool isHighlighted, bool isDown) override
    {
        auto colour = backgroundColour;
        if (isDown)
            colour = colour.darker(0.2f);
        else if (isHighlighted)
            colour = colour.brighter(0.12f);

        const auto bounds = button.getLocalBounds().toFloat().reduced(1.0f);
        g.setColour(colour);
        g.fillRect(bounds);
        g.setColour(palette::panelEdge);
        g.drawRect(bounds, 1.0f);
    }

    // A ring of square segments, like an LED meter, with one per step of travel.
    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float position,
                          float startAngle, float endAngle, juce::Slider&) override
    {
        const auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat();
        const auto centre = bounds.getCentre();
        const float radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) / 2.0f - 1.0f;
        constexpr int segments = 16;
        const int lit = juce::roundToInt(position * segments);
        const float size = juce::jmax(3.0f, radius * 0.16f);

        for (int i = 0; i <= segments; ++i)
        {
            const float angle = startAngle + (endAngle - startAngle) * static_cast<float>(i) / segments;
            const auto point = centre.getPointOnCircumference(radius - size * 0.6f, angle);
            g.setColour(i <= lit && position > 0.0f ? palette::accent : palette::panelEdge);
            g.fillRect(juce::Rectangle<float>(size, size).withCentre(point));
        }

        const float knobRadius = radius * 0.62f;
        g.setColour(palette::panelEdge);
        g.fillEllipse(juce::Rectangle<float>(knobRadius * 2.0f, knobRadius * 2.0f).withCentre(centre));
        g.setColour(palette::panel.brighter(0.05f));
        g.fillEllipse(juce::Rectangle<float>(knobRadius * 2.0f - 3.0f, knobRadius * 2.0f - 3.0f).withCentre(centre));

        const float angle = startAngle + (endAngle - startAngle) * position;
        g.setColour(palette::text);
        g.drawLine(juce::Line<float>(centre.getPointOnCircumference(knobRadius * 0.35f, angle),
                                     centre.getPointOnCircumference(knobRadius * 0.9f, angle)),
                   2.5f);
    }
};
} // namespace retrochip
