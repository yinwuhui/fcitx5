/*
 * SPDX-FileCopyrightText: 2017-2017 CSSlayer <wengxt@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 */
#include "inputwindow.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <fstream>
#include <exception>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <cairo.h>
#include <pango/pango-attributes.h>
#include <pango/pango-context.h>
#include <pango/pango-font.h>
#include <pango/pango-fontmap.h>
#include <pango/pango-language.h>
#include <pango/pango-layout.h>
#include <pango/pango-matrix.h>
#include <pango/pango-types.h>
#include <pango/pangocairo.h>
#include <yoga/YGEnums.h>
#include <yoga/YGNode.h>
#include <yoga/YGNodeLayout.h>
#include <yoga/YGNodeStyle.h>
#include "fcitx-utils/color.h"
#include "fcitx-utils/log.h"
#include "fcitx-utils/misc.h"
#include "fcitx-utils/rect.h"
#include "fcitx-utils/standardpaths.h"
#include "fcitx-utils/textformatflags.h"
#include "fcitx/action.h"
#include "fcitx/candidatelist.h"
#include "fcitx/inputmethodentry.h"
#include "fcitx/inputpanel.h"
#include "fcitx/instance.h"
#include "fcitx/misc_p.h"
#include "fcitx/text.h"
#include "fcitx/userinterface.h"
#include "fcitx/userinterfacemanager.h"
#include "classicui.h"
#include "common.h"
#include "theme.h"

namespace fcitx::classicui {

namespace {

constexpr std::array<std::string_view, 5> EmojiCategoryLabels = {
    "最近", "笑脸", "人物", "动物", "食物"};

const std::array<std::vector<std::string>, 4> EmojiCategories = {{
    {"😀", "😃", "😄", "😁", "😂", "🥰", "😍", "😊", "😉", "😎",
     "🤔", "😭", "😡", "🥳", "🤩"},
    {"👋", "👍", "👏", "🙏", "💪", "🤝", "👌", "✌️", "🤞", "🫶",
     "👨", "👩", "👶", "🧑‍💻", "🧑‍🎨"},
    {"🐶", "🐱", "🐭", "🐹", "🐰", "🦊", "🐻", "🐼", "🐨", "🐯",
     "🦁", "🐮", "🐷", "🐸", "🐵"},
    {"🍎", "🍊", "🍋", "🍉", "🍇", "🍓", "🍒", "🍑", "🥭", "🍍",
     "🍔", "🍕", "🍜", "🍰", "☕"},
}};

void roundedRectangle(cairo_t *cr, double x, double y, double width,
                      double height, double radius) {
    constexpr double Kappa = 0.5522847498307936;
    radius = std::min({radius, width / 2.0, height / 2.0});
    cairo_new_sub_path(cr);
    cairo_move_to(cr, x + radius, y);
    cairo_line_to(cr, x + width - radius, y);
    cairo_curve_to(cr, x + width - radius + radius * Kappa, y,
                   x + width, y + radius - radius * Kappa, x + width,
                   y + radius);
    cairo_line_to(cr, x + width, y + height - radius);
    cairo_curve_to(cr, x + width, y + height - radius + radius * Kappa,
                   x + width - radius + radius * Kappa, y + height,
                   x + width - radius, y + height);
    cairo_line_to(cr, x + radius, y + height);
    cairo_curve_to(cr, x + radius - radius * Kappa, y + height, x,
                   y + height - radius + radius * Kappa, x,
                   y + height - radius);
    cairo_line_to(cr, x, y + radius);
    cairo_curve_to(cr, x, y + radius - radius * Kappa,
                   x + radius - radius * Kappa, y, x + radius, y);
    cairo_close_path(cr);
}

void drawBubbleFishIcon(cairo_t *cr, Theme &theme, const Rect &region,
                        const std::string &name) {
    const uint32_t size =
        std::max(16, std::min(region.width(), region.height()) - 8);
    const auto &image = theme.loadBubbleFishIcon(name, size);
    if (!image.valid()) {
        return;
    }
    const double imageScale =
        std::min(static_cast<double>(region.width() - 8) / image.width(),
                 static_cast<double>(region.height() - 8) / image.height());
    const double x = region.left() +
                     (region.width() - image.width() * imageScale) / 2.0;
    const double y = region.top() +
                     (region.height() - image.height() * imageScale) / 2.0;
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, imageScale, imageScale);
    cairo_set_source_surface(cr, image, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
}

auto newPangoLayout(PangoContext *context) {
    GObjectUniquePtr<PangoLayout> ptr(pango_layout_new(context));
    pango_layout_set_single_paragraph_mode(ptr.get(), false);
    return ptr;
}

void prepareLayout(cairo_t *cr, PangoLayout *layout) {
    const PangoMatrix *matrix;

    matrix = pango_context_get_matrix(pango_layout_get_context(layout));

    if (matrix) {
        cairo_matrix_t cairo_matrix;

        cairo_matrix_init(&cairo_matrix, matrix->xx, matrix->yx, matrix->xy,
                          matrix->yy, matrix->x0, matrix->y0);

        cairo_transform(cr, &cairo_matrix);
    }
}

void renderLayout(cairo_t *cr, PangoLayout *layout, int x, int y) {
    auto *context = pango_layout_get_context(layout);
    const auto *fontDescription = pango_layout_get_font_description(layout);
    if (!fontDescription) {
        fontDescription = pango_context_get_font_description(context);
    }
    auto *metrics = pango_context_get_metrics(
        context, fontDescription, pango_context_get_language(context));
    auto ascent = pango_font_metrics_get_ascent(metrics);
    pango_font_metrics_unref(metrics);
    auto baseline = pango_layout_get_baseline(layout);
    auto yOffset = PANGO_PIXELS(ascent - baseline);
    cairo_save(cr);

    // Ensure the text are not painting on half pixel.
    cairo_move_to(cr, x, y + yOffset);
    double dx;
    double dy;
    double odx;
    double ody;
    cairo_get_current_point(cr, &dx, &dy);
    // Save old user value
    odx = dx;
    ody = dy;
    // Convert to device and round.
    cairo_user_to_device(cr, &dx, &dy);
    double ndx = std::round(dx);
    double ndy = std::round(dy);
    // Convert back to user and calculate delta.
    cairo_device_to_user(cr, &ndx, &ndy);
    cairo_move_to(cr, x + ndx - odx, y + yOffset + ndy - ody);

    prepareLayout(cr, layout);
    pango_cairo_show_layout(cr, layout);

    cairo_restore(cr);
}

} // namespace

int MultilineLayout::width() const {
    int width = 0;
    for (const auto &layout : lines_) {
        int w;
        int h;
        pango_layout_get_pixel_size(layout.get(), &w, &h);
        width = std::max(width, w);
    }
    return width;
}

void MultilineLayout::render(cairo_t *cr, int x, int y, bool highlight) {
    int lineHeight = fontHeight();
    for (size_t i = 0; i < lines_.size(); i++) {
        if (highlight) {
            pango_layout_set_attributes(lines_[i].get(),
                                        highlightAttrLists_[i].get());
        } else {
            pango_layout_set_attributes(lines_[i].get(), attrLists_[i].get());
        }
        renderLayout(cr, lines_[i].get(), x, y);
        y += lineHeight;
    }
}

InputWindow::InputWindow(ClassicUI *parent) : parent_(parent) {
    fontMap_.reset(pango_cairo_font_map_new());
    // Although documentation says it is 96 by default, try not rely on this
    // behavior.
    fontMapDefaultDPI_ = pango_cairo_font_map_get_resolution(
        PANGO_CAIRO_FONT_MAP(fontMap_.get()));
    context_.reset(pango_font_map_create_context(fontMap_.get()));
    upperLayout_ = newPangoLayout(context_.get());
    lowerLayout_ = newPangoLayout(context_.get());

    rootNode_.reset(YGNodeNew());
    mainNode_.reset(YGNodeNew());

    upperNode_.reset(YGNodeNew());
    upperTextNode_.reset(YGNodeNew());

    lowerNode_.reset(YGNodeNew());
    auxDownNode_.reset(YGNodeNew());
    auxDownTextNode_.reset(YGNodeNew());
    candidatesNode_.reset(YGNodeNew());

    buttonNode_.reset(YGNodeNew());

    YGNodeInsertChild(rootNode_.get(), mainNode_.get(), 0);
    YGNodeInsertChild(rootNode_.get(), buttonNode_.get(), 1);
    YGNodeStyleSetFlexDirection(rootNode_.get(), YGFlexDirectionRow);

    YGNodeInsertChild(mainNode_.get(), upperNode_.get(), 0);
    YGNodeInsertChild(mainNode_.get(), lowerNode_.get(), 1);
    toolBarNode_.reset(YGNodeNew());
    YGNodeInsertChild(mainNode_.get(), toolBarNode_.get(), 2);
    emojiPanelNode_.reset(YGNodeNew());
    YGNodeInsertChild(mainNode_.get(), emojiPanelNode_.get(), 3);
    YGNodeStyleSetFlexDirection(mainNode_.get(), YGFlexDirectionColumn);

    YGNodeInsertChild(upperNode_.get(), upperTextNode_.get(), 0);

    YGNodeInsertChild(lowerNode_.get(), auxDownNode_.get(), 0);
    YGNodeInsertChild(auxDownNode_.get(), auxDownTextNode_.get(), 0);

    YGNodeInsertChild(lowerNode_.get(), candidatesNode_.get(), 1);
    loadBubbleFishSettings();
    loadRecentEmojis();
}

void InputWindow::loadBubbleFishSettings() {
    const auto path = StandardPaths::global().userDirectory(
                          StandardPathsType::Config) /
                      "BubbleFish" / "BubbleFish Settings.conf";
    std::ifstream stream(path);
    std::string section;
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        const auto separator = line.find('=');
        if (separator == std::string::npos || section != "emoji") continue;
        const auto key = line.substr(0, separator);
        const auto value = line.substr(separator + 1);
        if (key == "enabled") emojiEnabled_ = value == "true" || value == "1";
        else if (key == "recent_limit") {
            try { emojiRecentLimit_ = std::min<size_t>(100, std::stoul(value)); }
            catch (const std::exception &) { emojiRecentLimit_ = 30; }
        } else if (key == "default_category") {
            constexpr std::array<std::string_view, 5> categories = {
                "recent", "smileys", "people", "animals", "food"};
            const auto found = std::find(categories.begin(), categories.end(), value);
            if (found != categories.end()) emojiCategory_ = std::distance(categories.begin(), found);
        }
    }
}

void InputWindow::loadRecentEmojis() {
    const auto path = StandardPaths::global().userDirectory(
                          StandardPathsType::Config) /
                      "bubblefish" / "emoji-recent";
    std::ifstream stream(path);
    std::string emoji;
    while (recentEmojis_.size() < emojiRecentLimit_ && std::getline(stream, emoji)) {
        if (!emoji.empty() &&
            std::find(recentEmojis_.begin(), recentEmojis_.end(), emoji) ==
                recentEmojis_.end()) {
            recentEmojis_.push_back(emoji);
        }
    }
}

void InputWindow::rememberEmoji(const std::string &emoji) {
    std::erase(recentEmojis_, emoji);
    recentEmojis_.push_front(emoji);
    while (recentEmojis_.size() > emojiRecentLimit_) {
        recentEmojis_.pop_back();
    }

    const auto directory = StandardPaths::global().userDirectory(
                               StandardPathsType::Config) /
                           "bubblefish";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    std::ofstream stream(directory / "emoji-recent", std::ios::trunc);
    for (const auto &recent : recentEmojis_) {
        stream << recent << '\n';
    }
}

void InputWindow::insertAttr(PangoAttrList *attrList, TextFormatFlags format,
                             int start, int end, bool highlight,
                             TextType type) const {
    const bool selectedCandidate =
        type == TextType::Regular &&
        (highlight || format.test(TextFormatFlag::HighLight));
    if (format & TextFormatFlag::Underline) {
        auto *attr = pango_attr_underline_new(PANGO_UNDERLINE_SINGLE);
        attr->start_index = start;
        attr->end_index = end;
        pango_attr_list_insert(attrList, attr);
    }
    if (format & TextFormatFlag::Italic) {
        auto *attr = pango_attr_style_new(PANGO_STYLE_ITALIC);
        attr->start_index = start;
        attr->end_index = end;
        pango_attr_list_insert(attrList, attr);
    }
    if (format & TextFormatFlag::Strike) {
        auto *attr = pango_attr_strikethrough_new(true);
        attr->start_index = start;
        attr->end_index = end;
        pango_attr_list_insert(attrList, attr);
    }
    if (format & TextFormatFlag::Bold) {
        auto *attr = pango_attr_weight_new(PANGO_WEIGHT_BOLD);
        attr->start_index = start;
        attr->end_index = end;
        pango_attr_list_insert(attrList, attr);
    }
    if (selectedCandidate) {
        // Make the selected candidate two typographic steps larger without
        // changing the configured base font size.
        auto *attr = pango_attr_scale_new(1.2);
        attr->start_index = start;
        attr->end_index = end;
        pango_attr_list_insert(attrList, attr);
    }
    Color color;
    if (selectedCandidate) {
        // Midpoint of BubbleFish logo's primary blue gradient.
        color = Color("#087CF2");
    } else if (format & TextFormatFlag::HighLight) {
        color = parent_->theme().inputPanelHighlightText();
    } else {
        Color table[2][3] = {
            {parent_->theme().inputPanelCandidateLabelText(),
             parent_->theme().inputPanelText(),
             parent_->theme().inputPanelCandidateCommentText()},
            {parent_->theme().inputPanelHighlightCandidateLabelText(),
             parent_->theme().inputPanelHighlightCandidateText(),
             parent_->theme().inputPanelHighlightCandidateCommentText()}};
        color = table[highlight][static_cast<int>(type)];
    }
    const auto scale = std::numeric_limits<uint16_t>::max();
    auto *attr = pango_attr_foreground_new(
        color.redF() * scale, color.greenF() * scale, color.blueF() * scale);
    attr->start_index = start;
    attr->end_index = end;
    pango_attr_list_insert(attrList, attr);

    if (color.alpha() != 255) {
        auto *alphaAttr =
            pango_attr_foreground_alpha_new(color.alphaF() * scale);
        alphaAttr->start_index = start;
        alphaAttr->end_index = end;
        pango_attr_list_insert(attrList, alphaAttr);
    }

}

void InputWindow::appendText(std::string &s, PangoAttrList *attrList,
                             PangoAttrList *highlightAttrList, const Text &text,
                             TextType type) {
    for (size_t i = 0, e = text.size(); i < e; i++) {
        auto start = s.size();
        s.append(text.stringAt(i));
        auto end = s.size();
        if (start == end) {
            continue;
        }
        const auto format = text.formatAt(i);
        insertAttr(attrList, format, start, end, false, type);
        if (highlightAttrList) {
            insertAttr(highlightAttrList, format, start, end, true, type);
        }
    }
}

void InputWindow::resizeCandidates(size_t n) {
    while (candidateLayouts_.size() < n) {
        candidateLayouts_.emplace_back();
    }

    nCandidates_ = n;
}

void InputWindow::setTextToMultilineLayout(InputContext *inputContext,
                                           MultilineLayout &layout,
                                           const Text &text, TextType type) {
    auto lines = text.splitByLine();
    layout.lines_.clear();
    layout.attrLists_.clear();
    layout.highlightAttrLists_.clear();

    for (const auto &line : lines) {
        layout.lines_.emplace_back(pango_layout_new(context_.get()));
        if (type == TextType::Regular &&
            line.toString().find('\t') != std::string::npos) {
            // Keep expanded columns aligned without the excessive fixed gap.
            // A numbered two-character candidate plus one character of space.
            auto *tabs = pango_tab_array_new(10, true);
            for (int column = 0; column < 10; ++column) {
                pango_tab_array_set_tab(tabs, column, PANGO_TAB_LEFT,
                                        (column + 1) * 64);
            }
            pango_layout_set_tabs(layout.lines_.back().get(), tabs);
            pango_tab_array_free(tabs);
        }
        layout.attrLists_.emplace_back();
        layout.highlightAttrLists_.emplace_back();
        setTextToLayout(inputContext, layout.lines_.back().get(),
                        &layout.attrLists_.back(),
                        &layout.highlightAttrLists_.back(), {line}, type);
    }
}

void InputWindow::setTextToLayout(
    InputContext *inputContext, PangoLayout *layout,
    PangoAttrListUniquePtr *attrList, PangoAttrListUniquePtr *highlightAttrList,
    std::initializer_list<std::reference_wrapper<const Text>> texts,
    TextType type) {
    auto *newAttrList = pango_attr_list_new();
    if (attrList) {
        // PangoAttrList does not have "clear()". So when we set new text,
        // we need to create a new one and get rid of old one.
        // We keep a ref to the attrList.
        attrList->reset(pango_attr_list_ref(newAttrList));
    }
    PangoAttrList *newHighlightAttrList = nullptr;
    if (highlightAttrList) {
        newHighlightAttrList = pango_attr_list_new();
        highlightAttrList->reset(newHighlightAttrList);
    }
    std::string line;
    for (const auto &text : texts) {
        appendText(line, newAttrList, newHighlightAttrList, text, type);
    }

    const auto *entry = parent_->instance()->inputMethodEntry(inputContext);
    if (*parent_->config().useInputMethodLanguageToDisplayText && entry &&
        !entry->languageCode().empty()) {
        if (auto *language =
                pango_language_from_string(entry->languageCode().c_str())) {
            if (newAttrList) {
                auto *attr = pango_attr_language_new(language);
                attr->start_index = 0;
                attr->end_index = line.size();
                pango_attr_list_insert(newAttrList, attr);
            }
            if (newHighlightAttrList) {
                auto *attr = pango_attr_language_new(language);
                attr->start_index = 0;
                attr->end_index = line.size();
                pango_attr_list_insert(newHighlightAttrList, attr);
            }
        }
    }

    pango_layout_set_text(layout, line.c_str(), line.size());
    pango_layout_set_attributes(layout, newAttrList);
    pango_attr_list_unref(newAttrList);
}

std::pair<int, int> InputWindow::update(InputContext *inputContext) {
    hoverIndex_ = -1;
    voicePanel_ = false;
    voiceCloseRegion_ = Rect();
    if ((parent_->suspended() &&
         parent_->instance()->currentUI() != "kimpanel") ||
        !inputContext) {
        visible_ = false;
        return {0, 0};
    }
    // | aux up | preedit
    // | aux down
    // | 1 candidate | 2 ...
    // or
    // | aux up | preedit
    // | aux down
    // | candidate 1
    // | candidate 2
    // | candidate 3
    auto *instance = parent_->instance();
    auto &inputPanel = inputContext->inputPanel();
    inputContext_ = inputContext->watch();

    const auto rawAuxUp = inputPanel.auxUp().toString();
    constexpr std::string_view VoiceMarker = "\x1f"
                                             "BFVOICE|";
    if (rawAuxUp.starts_with(VoiceMarker)) {
        voicePanel_ = true;
        voiceLevels_.clear();
        const auto payload = rawAuxUp.substr(VoiceMarker.size());
        const auto levelSeparator = payload.find('|');
        const auto selectionSeparator =
            levelSeparator == std::string::npos
                ? std::string::npos
                : payload.find('|', levelSeparator + 1);
        const auto levelText = payload.substr(0, levelSeparator);
        try {
            voiceSelection_ = std::max(
                0, std::stoi(payload.substr(
                       levelSeparator + 1,
                       selectionSeparator - levelSeparator - 1)));
        } catch (const std::exception &) {
            voiceSelection_ = 0;
        }
        const auto recognizedText =
            selectionSeparator == std::string::npos
                ? std::string()
                : payload.substr(selectionSeparator + 1);
        voiceText_.clear();
        int selectedStart = -1;
        int selectedEnd = -1;
        std::stringstream candidateStream(recognizedText);
        std::string candidate;
        int candidateIndex = 0;
        while (std::getline(candidateStream, candidate)) {
            if (candidate.empty()) continue;
            if (!voiceText_.empty()) voiceText_.push_back('\n');
            const int lineStart = static_cast<int>(voiceText_.size());
            voiceText_ += candidateIndex == voiceSelection_ ? "› " : "  ";
            voiceText_ += std::to_string(candidateIndex + 1) + ". " + candidate;
            if (candidateIndex == voiceSelection_) {
                selectedStart = lineStart;
                selectedEnd = static_cast<int>(voiceText_.size());
            }
            ++candidateIndex;
        }
        size_t begin = 0;
        while (begin < levelText.size()) {
            const auto end = levelText.find(',', begin);
            try {
                voiceLevels_.push_back(std::clamp(
                    std::stod(levelText.substr(begin, end - begin)), 0.0, 1.0));
            } catch (const std::exception &) {
                voiceLevels_.push_back(0.0);
            }
            if (end == std::string::npos) {
                break;
            }
            begin = end + 1;
        }
        if (voiceText_.empty()) voiceText_ = "  1. 正在聆听…";
        auto *fontDesc = pango_font_description_from_string(
            parent_->config().font->c_str());
        pango_context_set_font_description(context_.get(), fontDesc);
        pango_font_description_free(fontDesc);
        pango_layout_set_single_paragraph_mode(upperLayout_.get(), false);
        pango_layout_set_width(upperLayout_.get(), 370 * PANGO_SCALE);
        pango_layout_set_wrap(upperLayout_.get(), PANGO_WRAP_WORD_CHAR);
        pango_layout_set_ellipsize(upperLayout_.get(), PANGO_ELLIPSIZE_END);
        pango_layout_set_height(upperLayout_.get(), -5);
        pango_layout_set_text(upperLayout_.get(), voiceText_.c_str(), -1);
        auto *voiceAttributes = pango_attr_list_new();
        if (selectedStart >= 0 && selectedEnd > selectedStart) {
            auto *color = pango_attr_foreground_new(8 * 257, 124 * 257,
                                                     242 * 257);
            color->start_index = selectedStart;
            color->end_index = selectedEnd;
            pango_attr_list_insert(voiceAttributes, color);
            auto *scale = pango_attr_scale_new(1.2);
            scale->start_index = selectedStart;
            scale->end_index = selectedEnd;
            pango_attr_list_insert(voiceAttributes, scale);
        }
        pango_layout_set_attributes(upperLayout_.get(), voiceAttributes);
        pango_attr_list_unref(voiceAttributes);
        visible_ = true;
        int textHeight = 0;
        pango_layout_get_pixel_size(upperLayout_.get(), nullptr, &textHeight);
        return {420, std::max(50, 24 + textHeight)};
    }

    cursor_ = -1;
    auto preedit = instance->outputFilter(inputContext, inputPanel.preedit());
    auto auxUp = instance->outputFilter(inputContext, inputPanel.auxUp());
    pango_layout_set_single_paragraph_mode(upperLayout_.get(), true);
    pango_layout_set_width(upperLayout_.get(), -1);
    pango_layout_set_height(upperLayout_.get(), -1);
    pango_layout_set_ellipsize(upperLayout_.get(), PANGO_ELLIPSIZE_NONE);
    setTextToLayout(inputContext, upperLayout_.get(), nullptr, nullptr,
                    {auxUp, preedit});
    if (preedit.cursor() >= 0 &&
        static_cast<size_t>(preedit.cursor()) <= preedit.textLength()) {
        cursor_ = preedit.cursor() + auxUp.toString().size();
    }

    auto auxDown = instance->outputFilter(inputContext, inputPanel.auxDown());
    setTextToLayout(inputContext, lowerLayout_.get(), nullptr, nullptr,
                    {auxDown});

    if (auto candidateList = inputPanel.candidateList()) {
        // Count non-placeholder candidates.
        int count = 0;

        for (int i = 0, e = candidateList->size(); i < e; i++) {
            const auto &candidate = candidateList->candidate(i);
            if (candidate.isPlaceHolder()) {
                continue;
            }
            count++;
        }
        resizeCandidates(count);

        candidateIndex_ = -1;
        int localIndex = 0;
        for (int i = 0, e = candidateList->size(); i < e; i++) {
            const auto &candidate = candidateList->candidate(i);
            // Skip placeholder.
            if (candidate.isPlaceHolder()) {
                continue;
            }

            if (i == candidateList->cursorIndex()) {
                candidateIndex_ = localIndex;
            }

            Text labelText = candidate.hasCustomLabel()
                                 ? candidate.customLabel()
                                 : candidateList->label(i);

            labelText = instance->outputFilter(inputContext, labelText);
            setTextToMultilineLayout(inputContext,
                                     candidateLayouts_[localIndex].label,
                                     labelText, TextType::Label);
            auto candidateText =
                instance->outputFilter(inputContext, candidate.text());
            setTextToMultilineLayout(inputContext,
                                     candidateLayouts_[localIndex].text,
                                     candidateText, TextType::Regular);
            auto realCommentText =
                instance->outputFilter(inputContext, candidate.comment());
            Text commentText;
            if (!realCommentText.empty()) {
                if (candidate.spaceBetweenComment()) {
                    // Still need some extra space before comment, let's just
                    // add a space and see.
                    commentText = Text(" ");
                }
                commentText.append(realCommentText);
            }
            setTextToMultilineLayout(inputContext,
                                     candidateLayouts_[localIndex].comment,
                                     commentText, TextType::Comment);
            localIndex++;
        }

        layoutHint_ = candidateList->layoutHint();
        if (auto *pageable = candidateList->toPageable()) {
            hasPrev_ = pageable->hasPrev();
            hasNext_ = pageable->hasNext();
        } else {
            hasPrev_ = false;
            hasNext_ = false;
        }
    } else {
        nCandidates_ = 0;
        candidateIndex_ = -1;
        hasPrev_ = false;
        hasNext_ = false;
    }

    visible_ = nCandidates_ ||
               pango_layout_get_character_count(upperLayout_.get()) ||
               pango_layout_get_character_count(lowerLayout_.get());
    int width = 0;
    int height = 0;
    if (visible_) {
        std::tie(width, height) = sizeHint();
        if (width <= 0 || height <= 0) {
            width = height = 0;
            visible_ = false;
        }
    }
    return {width, height};
}

std::pair<unsigned int, unsigned int> InputWindow::sizeHint() {
    auto &theme = parent_->theme();
    auto *fontDesc =
        pango_font_description_from_string(parent_->config().font->c_str());
    pango_context_set_font_description(context_.get(), fontDesc);
    pango_layout_context_changed(upperLayout_.get());
    pango_layout_context_changed(lowerLayout_.get());
    auto originalSize = pango_font_description_get_size(fontDesc);
    auto labelSize =
        originalSize * (*theme.inputPanel->labelTextSizeFactor / 100.0);
    auto commentSize =
        originalSize * (*theme.inputPanel->commentTextSizeFactor / 100.0);
    for (size_t i = 0; i < nCandidates_; i++) {

        candidateLayouts_[i].label.contextChanged();
        candidateLayouts_[i].text.contextChanged();
        candidateLayouts_[i].comment.contextChanged();
        if (originalSize > 0) {
            // For candidate, we use a smaller font size.
            pango_font_description_set_size(fontDesc, labelSize);
            candidateLayouts_[i].label.setFontDescription(fontDesc);

            // For candidate, we use a smaller font size.
            pango_font_description_set_size(fontDesc, commentSize);
            candidateLayouts_[i].comment.setFontDescription(fontDesc);
        } else {
            candidateLayouts_[i].label.setFontDescription(nullptr);
            candidateLayouts_[i].comment.setFontDescription(nullptr);
        }
    }
    pango_font_description_free(fontDesc);

    // Update yoga layout to calculate dimensions
    updateYogaLayout();

    // Get dimensions from yoga layout
    float yogaWidth = YGNodeLayoutGetWidth(rootNode_.get());
    float yogaHeight = YGNodeLayoutGetHeight(rootNode_.get());

    auto width = static_cast<unsigned int>(yogaWidth);
    auto height = static_cast<unsigned int>(yogaHeight);
    return {width, height};
}

void InputWindow::paint(cairo_t *cr, unsigned int width, unsigned int height,
                        double scale) {
    cairo_scale(cr, scale, scale);
    auto &theme = parent_->theme();
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    constexpr double CornerRadius = 12.0;
    const double borderWidth = *theme.inputPanel->background->borderWidth;
    cairoSetSourceColor(cr, Color("#087CF2"));
    roundedRectangle(cr, 0, 0, width, height, CornerRadius);
    cairo_fill(cr);
    cairoSetSourceColor(cr, theme.inputPanelBackground());
    roundedRectangle(cr, borderWidth, borderWidth,
                     width - 2 * borderWidth, height - 2 * borderWidth,
                     std::max(0.0, CornerRadius - borderWidth));
    cairo_fill(cr);
    const auto &margin = *theme.inputPanel->contentMargin;
    const auto &textMargin = *theme.inputPanel->textMargin;
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_save(cr);

    cairoSetSourceColor(cr, theme.inputPanelText());
    if (voicePanel_) {
        constexpr double AccentRed = 8.0 / 255.0;
        constexpr double AccentGreen = 124.0 / 255.0;
        constexpr double AccentBlue = 242.0 / 255.0;

        voiceCloseRegion_.setPosition(382, 10);
        voiceCloseRegion_.setSize(28, 28);
        cairo_set_source_rgba(cr, AccentRed, AccentGreen, AccentBlue, 0.12);
        roundedRectangle(cr, voiceCloseRegion_.left(),
                         voiceCloseRegion_.top(), voiceCloseRegion_.width(),
                         voiceCloseRegion_.height(), 8);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, AccentRed, AccentGreen, AccentBlue);
        cairo_set_line_width(cr, 2.0);
        cairo_move_to(cr, 390, 18);
        cairo_line_to(cr, 402, 30);
        cairo_move_to(cr, 402, 18);
        cairo_line_to(cr, 390, 30);
        cairo_stroke(cr);

        cairoSetSourceColor(cr, theme.inputPanelText());
        renderLayout(cr, upperLayout_.get(), 18, 12);
        cairo_restore(cr);
        return;
    }

    // CLASSICUI_DEBUG() << theme.inputPanel->normalColor->toString();
    auto *metrics = pango_context_get_metrics(
        context_.get(), pango_context_get_font_description(context_.get()),
        pango_context_get_language(context_.get()));
    auto fontHeight = pango_font_metrics_get_ascent(metrics) +
                      pango_font_metrics_get_descent(metrics);
    pango_font_metrics_unref(metrics);
    fontHeight = PANGO_PIXELS(fontHeight);

    // Use yoga-based positioning for upper layout
    if (pango_layout_get_character_count(upperLayout_.get())) {
        float upperLeft = absolute<YGNodeLayoutGetLeft>(upperTextNode_);
        float upperTop = absolute<YGNodeLayoutGetTop>(upperTextNode_);

        renderLayout(cr, upperLayout_.get(), upperLeft, upperTop);

        PangoRectangle pos;
        if (cursor_ >= 0) {
            pango_layout_get_cursor_pos(upperLayout_.get(), cursor_, &pos,
                                        nullptr);

            cairo_save(cr);
            cairo_set_line_width(cr, 2);
            auto offsetX = pango_units_to_double(pos.x);
            cairo_move_to(cr, upperLeft + offsetX + 1, upperTop);
            cairo_line_to(cr, upperLeft + offsetX + 1, upperTop + fontHeight);
            cairo_stroke(cr);
            cairo_restore(cr);
        }
        int upperHeight = 0;
        pango_layout_get_pixel_size(upperLayout_.get(), nullptr, &upperHeight);
        const int actionHeight = std::max(28, upperHeight + 6);
        const int actionRight =
            width - std::max(0.0, borderWidth) - *margin.marginRight -
            *textMargin.marginRight;
        voiceRegion_.setPosition(actionRight - 28, upperTop - 3);
        voiceRegion_.setSize(28, actionHeight);
        emojiRegion_.setPosition(voiceRegion_.left() - 32,
                                 voiceRegion_.top());
        emojiRegion_.setSize(28, actionHeight);
        if (emojiEnabled_) {
            drawBubbleFishIcon(cr, theme, emojiRegion_, "bubblefish-emoji");
        }
        drawBubbleFishIcon(cr, theme, voiceRegion_, "bubblefish-voice");
    } else {
        emojiRegion_ = Rect();
        voiceRegion_ = Rect();
    }

    // Use yoga-based positioning for lower layout
    if (pango_layout_get_character_count(lowerLayout_.get())) {
        float lowerLeft = absolute<YGNodeLayoutGetLeft>(auxDownTextNode_);
        float lowerTop = absolute<YGNodeLayoutGetTop>(auxDownTextNode_);

        renderLayout(cr, lowerLayout_.get(), lowerLeft, lowerTop);
    }

    candidateRegions_.clear();
    candidateRegions_.reserve(nCandidates_);
    candidateTextLefts_.clear();
    candidateTextLefts_.reserve(nCandidates_);

    // Use yoga-based positioning for candidates
    for (size_t i = 0; i < nCandidates_; i++) {
        float labelLeft =
            absolute<YGNodeLayoutGetLeft>(candidateNodes_[i].label);
        float labelTop = absolute<YGNodeLayoutGetTop>(candidateNodes_[i].label);

        float textLeft = absolute<YGNodeLayoutGetLeft>(candidateNodes_[i].text);
        float textTop = absolute<YGNodeLayoutGetTop>(candidateNodes_[i].text);
        candidateTextLefts_.push_back(static_cast<int>(textLeft));

        float commentLeft =
            absolute<YGNodeLayoutGetLeft>(candidateNodes_[i].comment);
        float commentTop =
            absolute<YGNodeLayoutGetTop>(candidateNodes_[i].comment);

        float candidateLeft =
            absolute<YGNodeLayoutGetLeft>(candidateNodes_[i].inner);
        float candidateTop =
            absolute<YGNodeLayoutGetTop>(candidateNodes_[i].inner);
        float candidateWidth =
            YGNodeLayoutGetWidth(candidateNodes_[i].inner.get());
        float candidateHeight =
            YGNodeLayoutGetHeight(candidateNodes_[i].inner.get());

        const auto &highlightMargin = *theme.inputPanel->highlight->margin;
        const auto &clickMargin = *theme.inputPanel->highlight->clickMargin;
        auto highlightWidth = candidateWidth;
        bool vertical = parent_->config().verticalCandidateList.value();
        if (layoutHint_ == CandidateLayoutHint::Vertical) {
            vertical = true;
        } else if (layoutHint_ == CandidateLayoutHint::Horizontal) {
            vertical = false;
        }

        if (*theme.inputPanel->fullWidthHighlight && vertical) {
            // Last candidate, fill.
            highlightWidth = width - *margin.marginLeft - *margin.marginRight -
                             *textMargin.marginRight - *textMargin.marginLeft;
        }

        const int highlightIndex = highlight();
        bool highlight = false;
        if (highlightIndex >= 0 && i == static_cast<size_t>(highlightIndex)) {
            highlight = true;
        }

        Rect candidateRegion;
        candidateRegion
            .setPosition(candidateLeft - *highlightMargin.marginLeft +
                             *clickMargin.marginLeft,
                         candidateTop - *highlightMargin.marginTop +
                             *clickMargin.marginTop)
            .setSize(highlightWidth + *highlightMargin.marginLeft +
                         *highlightMargin.marginRight -
                         *clickMargin.marginLeft - *clickMargin.marginRight,
                     candidateHeight + *highlightMargin.marginTop +
                         *highlightMargin.marginBottom -
                         *clickMargin.marginTop - *clickMargin.marginBottom);
        candidateRegions_.push_back(candidateRegion);

        if (candidateLayouts_[i].label.characterCount()) {
            candidateLayouts_[i].label.render(cr, labelLeft, labelTop,
                                              highlight);
        }
        if (candidateLayouts_[i].text.characterCount()) {
            candidateLayouts_[i].text.render(cr, textLeft, textTop, highlight);
        }
        if (candidateLayouts_[i].comment.characterCount()) {
            candidateLayouts_[i].comment.render(cr, commentLeft, commentTop,
                                                highlight);
        }
    }
    cairo_restore(cr);

    prevRegion_ = Rect();
    nextRegion_ = Rect();
    if (nCandidates_ && !candidateRegions_.empty()) {
        const int buttonY = candidateRegions_.front().top();
        const int buttonH = std::max(28, candidateRegions_.front().height());
        const int buttonX = absolute<YGNodeLayoutGetLeft>(buttonNode_);
        nextRegion_.setPosition(buttonX, buttonY);
        nextRegion_.setSize(30, buttonH);
        prevRegion_.setPosition(buttonX + 32, buttonY);
        prevRegion_.setSize(30, buttonH);
        drawBubbleFishIcon(cr, theme, nextRegion_, "bubblefish-dropdown");
        drawBubbleFishIcon(cr, theme, prevRegion_, "bubblefish-tools");
    }

    clipboardRegion_ = Rect();
    translateRegion_ = Rect();
    fullShapeRegion_ = Rect();
    if (showToolBar_) {
        const int toolX = absolute<YGNodeLayoutGetLeft>(toolBarNode_) + 6;
        const int toolY = absolute<YGNodeLayoutGetTop>(toolBarNode_) + 3;
        clipboardRegion_.setPosition(toolX, toolY);
        clipboardRegion_.setSize(36, 34);
        translateRegion_.setPosition(toolX + 44, toolY);
        translateRegion_.setSize(36, 34);
        fullShapeRegion_.setPosition(toolX + 88, toolY);
        fullShapeRegion_.setSize(36, 34);
        drawBubbleFishIcon(cr, theme, clipboardRegion_,
                           "bubblefish-clipboard");
        drawBubbleFishIcon(cr, theme, translateRegion_,
                           "bubblefish-translate");
        auto *fullShapeAction = parent_->instance()
                                    ->userInterfaceManager()
                                    .lookupAction("bubblefish-full-shape");
        const bool fullShape =
            fullShapeAction && fullShapeAction->isChecked(inputContext_.get());
        drawBubbleFishIcon(cr, theme, fullShapeRegion_,
                           fullShape ? "bubblefish-full-to-half"
                                     : "bubblefish-half-to-full");
    }

    emojiCategoryRegions_.clear();
    emojiItemRegions_.clear();
    visibleEmojis_.clear();
    if (showEmojiPanel_) {
        const int panelX = absolute<YGNodeLayoutGetLeft>(emojiPanelNode_) + 6;
        const int panelY = absolute<YGNodeLayoutGetTop>(emojiPanelNode_) + 4;
        auto textLayout = newPangoLayout(context_.get());

        for (size_t i = 0; i < EmojiCategoryLabels.size(); ++i) {
            Rect region;
            region.setPosition(panelX + static_cast<int>(i) * 58, panelY);
            region.setSize(54, 26);
            emojiCategoryRegions_.push_back(region);
            pango_layout_set_text(textLayout.get(),
                                  EmojiCategoryLabels[i].data(), -1);
            cairoSetSourceColor(cr, static_cast<int>(i) == emojiCategory_
                                        ? Color("#087CF2")
                                        : theme.inputPanelText());
            renderLayout(cr, textLayout.get(), region.left() + 8,
                         region.top() + 3);
            if (static_cast<int>(i) == emojiCategory_) {
                cairo_set_line_width(cr, 2);
                cairo_move_to(cr, region.left() + 7, region.bottom() - 1);
                cairo_line_to(cr, region.right() - 7, region.bottom() - 1);
                cairo_stroke(cr);
            }
        }

        if (emojiCategory_ == 0) {
            visibleEmojis_.assign(recentEmojis_.begin(), recentEmojis_.end());
        } else {
            visibleEmojis_ = EmojiCategories[emojiCategory_ - 1];
        }
        if (visibleEmojis_.size() > 10) {
            visibleEmojis_.resize(10);
        }

        auto *emojiFont = pango_font_description_copy(
            pango_context_get_font_description(context_.get()));
        pango_font_description_set_absolute_size(emojiFont, 18 * PANGO_SCALE);
        pango_layout_set_font_description(textLayout.get(), emojiFont);
        pango_font_description_free(emojiFont);
        cairoSetSourceColor(cr, theme.inputPanelText());
        for (size_t i = 0; i < visibleEmojis_.size(); ++i) {
            Rect region;
            region.setPosition(panelX + static_cast<int>(i) * 34, panelY + 32);
            region.setSize(32, 34);
            emojiItemRegions_.push_back(region);
            pango_layout_set_text(textLayout.get(), visibleEmojis_[i].c_str(),
                                  -1);
            renderLayout(cr, textLayout.get(), region.left() + 5,
                         region.top() + 4);
        }
        if (visibleEmojis_.empty()) {
            pango_layout_set_font_description(textLayout.get(), nullptr);
            pango_layout_set_text(textLayout.get(), "暂无最近使用", -1);
            cairoSetSourceColor(cr, theme.inputPanelText());
            renderLayout(cr, textLayout.get(), panelX + 8, panelY + 39);
        }
    }

    if (classicui_logcategory().checkLogLevel(Debug)) {
        renderYogaNode(cr, rootNode_.get());
    }
}

void InputWindow::click(int x, int y) {
    auto *inputContext = inputContext_.get();
    if (!inputContext) {
        return;
    }
    if (voicePanel_ && voiceCloseRegion_.contains(x, y)) {
        startProcess({"/usr/bin/bubblefish-voice-service", "--close"});
        return;
    }
    if (emojiEnabled_ && emojiRegion_.contains(x, y)) {
        showEmojiPanel_ = !showEmojiPanel_;
        inputContext->updateUserInterface(UserInterfaceComponent::InputPanel);
        return;
    }
    for (size_t i = 0; i < emojiCategoryRegions_.size(); ++i) {
        if (emojiCategoryRegions_[i].contains(x, y)) {
            emojiCategory_ = static_cast<int>(i);
            inputContext->updateUserInterface(
                UserInterfaceComponent::InputPanel);
            return;
        }
    }
    for (size_t i = 0; i < emojiItemRegions_.size(); ++i) {
        if (emojiItemRegions_[i].contains(x, y) &&
            i < visibleEmojis_.size()) {
            const auto emoji = visibleEmojis_[i];
            rememberEmoji(emoji);
            showEmojiPanel_ = false;
            inputContext->reset();
            inputContext->commitString(emoji);
            return;
        }
    }
    const auto candidateList = inputContext->inputPanel().candidateList();
    if (!candidateList) {
        return;
    }
    if (auto *grid = candidateList->toGrid()) {
        constexpr int GridColumnWidth = 64;
        for (size_t row = 0; row < candidateRegions_.size() &&
                             row < candidateTextLefts_.size();
             ++row) {
            if (!candidateRegions_[row].contains(x, y)) {
                continue;
            }
            const int relativeX = x - candidateTextLefts_[row];
            const int column = relativeX / GridColumnWidth;
            if (relativeX >= 0 &&
                column < grid->columnCount(static_cast<int>(row))) {
                grid->select(static_cast<int>(row), column, inputContext);
            }
            return;
        }
    }
    if (fullShapeRegion_.contains(x, y)) {
        if (auto *action = parent_->instance()
                               ->userInterfaceManager()
                               .lookupAction("bubblefish-full-shape")) {
            action->activate(inputContext);
        }
        return;
    }
    if (auto *pageable = candidateList->toPageable()) {
        if (prevRegion_.contains(x, y)) {
            showToolBar_ = !showToolBar_;
            inputContext->updateUserInterface(
                UserInterfaceComponent::InputPanel);
            return;
        }
        if (pageable->hasNext() && nextRegion_.contains(x, y)) {
            pageable->next();
            inputContext->updateUserInterface(
                UserInterfaceComponent::InputPanel);
            return;
        }
    }
    for (size_t idx = 0, e = candidateRegions_.size(); idx < e; idx++) {
        if (candidateRegions_[idx].contains(x, y)) {
            const auto *candidate =
                nthCandidateIgnorePlaceholder(*candidateList, idx);
            if (candidate) {
                candidate->select(inputContext);
            }
            break;
        }
    }
}

void InputWindow::wheel(bool up) {
    if (!*parent_->config().useWheelForPaging) {
        return;
    }
    auto *inputContext = inputContext_.get();
    if (!inputContext) {
        return;
    }
    const auto candidateList = inputContext->inputPanel().candidateList();
    if (!candidateList) {
        return;
    }
    if (auto *pageable = candidateList->toPageable()) {
        if (up) {
            if (pageable->hasPrev()) {
                pageable->prev();
                inputContext->updateUserInterface(
                    UserInterfaceComponent::InputPanel);
            }
        } else {
            if (pageable->hasNext()) {
                pageable->next();
                inputContext->updateUserInterface(
                    UserInterfaceComponent::InputPanel);
            }
        }
    }
}

void InputWindow::setFontDPI(int dpi) {
    // Unlike pango cairo context, Cairo font map does not accept negative dpi.
    // Restore to default value instead.
    if (dpi <= 0) {
        pango_cairo_font_map_set_resolution(
            PANGO_CAIRO_FONT_MAP(fontMap_.get()), fontMapDefaultDPI_);
    } else {
        pango_cairo_font_map_set_resolution(
            PANGO_CAIRO_FONT_MAP(fontMap_.get()), dpi);
    }
    pango_cairo_context_set_resolution(context_.get(), dpi);
}

int InputWindow::highlight() const {
    int highlightIndex = (hoverIndex_ >= 0) ? hoverIndex_ : candidateIndex_;
    return highlightIndex;
}

bool InputWindow::hover(int x, int y) {
    bool needRepaint = false;

    bool prevHovered = false;
    bool nextHovered = false;
    auto oldHighlight = highlight();
    hoverIndex_ = -1;

    prevHovered = prevRegion_.contains(x, y);
    if (!prevHovered) {
        nextHovered = nextRegion_.contains(x, y);
        if (!nextHovered) {
            for (int idx = 0, e = candidateRegions_.size(); idx < e; idx++) {
                if (candidateRegions_[idx].contains(x, y)) {
                    hoverIndex_ = idx;
                    break;
                }
            }
        }
    }

    needRepaint = needRepaint || prevHovered_ != prevHovered;
    prevHovered_ = prevHovered;

    needRepaint = needRepaint || nextHovered_ != nextHovered;
    nextHovered_ = nextHovered;

    needRepaint = needRepaint || oldHighlight != highlight();
    return needRepaint;
}

void InputWindow::updateYogaLayout() {
    auto &theme = parent_->theme();
    const auto &margin = *theme.inputPanel->contentMargin;
    const auto &textMargin = *theme.inputPanel->textMargin;

    // Get font metrics for calculations
    auto *metrics = pango_context_get_metrics(
        context_.get(), pango_context_get_font_description(context_.get()),
        pango_context_get_language(context_.get()));
    auto fontHeight = pango_font_metrics_get_ascent(metrics) +
                      pango_font_metrics_get_descent(metrics);
    pango_font_metrics_unref(metrics);
    fontHeight = PANGO_PIXELS(fontHeight);

    // Clean up candidate nodes
    YGNodeRemoveAllChildren(candidatesNode_.get());
    for (auto &node : candidateNodes_) {
        YGNodeRemoveAllChildren(node.self.get());
        YGNodeRemoveAllChildren(node.inner.get());
    }
    // Ensure candidate nodes vector has enough elements
    while (candidateNodes_.size() < nCandidates_) {
        candidateNodes_.emplace_back();
    }
    while (candidateNodes_.size() > nCandidates_) {
        candidateNodes_.pop_back();
    }

    // Configure root node
    YGNodeStyleSetPadding(rootNode_.get(), YGEdgeLeft, *margin.marginLeft);
    YGNodeStyleSetPadding(rootNode_.get(), YGEdgeRight, *margin.marginRight);
    YGNodeStyleSetPadding(rootNode_.get(), YGEdgeTop, *margin.marginTop);
    YGNodeStyleSetPadding(rootNode_.get(), YGEdgeBottom, *margin.marginBottom);

    // Configure and add upper node if it has content
    bool hasUpperContent =
        pango_layout_get_character_count(upperLayout_.get()) > 0;
    YGNodeStyleSetDisplay(upperNode_.get(),
                          hasUpperContent ? YGDisplayFlex : YGDisplayNone);
    if (hasUpperContent) {
        YGNodeStyleSetFlexDirection(upperNode_.get(), YGFlexDirectionColumn);
        YGNodeStyleSetPadding(upperNode_.get(), YGEdgeLeft,
                              *textMargin.marginLeft);
        YGNodeStyleSetPadding(upperNode_.get(), YGEdgeRight,
                              *textMargin.marginRight);
        YGNodeStyleSetPadding(upperNode_.get(), YGEdgeTop,
                              *textMargin.marginTop);
        YGNodeStyleSetPadding(upperNode_.get(), YGEdgeBottom,
                              *textMargin.marginBottom);

        int w;
        int h;
        pango_layout_get_pixel_size(upperLayout_.get(), &w, &h);
        YGNodeStyleSetWidth(upperTextNode_.get(), w);
        YGNodeStyleSetHeight(upperTextNode_.get(), fontHeight);
        // Reserve space for emoji and voice actions after the composition.
        YGNodeStyleSetWidth(upperNode_.get(), w + 72);
    }

    // Configure and add lower node if it has content
    bool hasAuxDown = pango_layout_get_character_count(lowerLayout_.get()) > 0;
    bool hasLowerContent = hasAuxDown || nCandidates_ > 0;
    YGNodeStyleSetDisplay(lowerNode_.get(),
                          hasLowerContent ? YGDisplayFlex : YGDisplayNone);
    YGNodeStyleSetDisplay(auxDownNode_.get(),
                          hasAuxDown ? YGDisplayFlex : YGDisplayNone);
    if (hasAuxDown) {
        YGNodeStyleSetPadding(auxDownNode_.get(), YGEdgeLeft,
                              *textMargin.marginLeft);
        YGNodeStyleSetPadding(auxDownNode_.get(), YGEdgeRight,
                              *textMargin.marginRight);
        YGNodeStyleSetPadding(auxDownNode_.get(), YGEdgeTop,
                              *textMargin.marginTop);
        YGNodeStyleSetPadding(auxDownNode_.get(), YGEdgeBottom,
                              *textMargin.marginBottom);

        int w;
        int h;
        pango_layout_get_pixel_size(lowerLayout_.get(), &w, &h);
        YGNodeStyleSetWidth(auxDownTextNode_.get(), w);
        YGNodeStyleSetHeight(auxDownTextNode_.get(), fontHeight);
    }

    // Add candidates node if there are candidates
    if (nCandidates_ > 0) {
        // Configure candidates node based on layout hint
        bool vertical = parent_->config().verticalCandidateList.value();
        if (layoutHint_ == CandidateLayoutHint::Vertical) {
            vertical = true;
        } else if (layoutHint_ == CandidateLayoutHint::Horizontal) {
            vertical = false;
        }

        YGNodeStyleSetFlexDirection(lowerNode_.get(),
                                    vertical ? YGFlexDirectionColumn
                                             : YGFlexDirectionRow);
        YGNodeStyleSetFlexDirection(candidatesNode_.get(),
                                    vertical ? YGFlexDirectionColumn
                                             : YGFlexDirectionRow);

        // Configure individual candidate nodes
        for (size_t i = 0; i < nCandidates_; i++) {
            int labelW = 0;
            int labelH = 0;
            int candidateW = 0;
            int candidateH = 0;
            int commentW = 0;
            int commentH = 0;

            int labelFontHeight = fontHeight;
            if (auto height = candidateLayouts_[i].label.fontHeight()) {
                labelFontHeight = height;
            }

            int commentFontHeight = fontHeight;
            if (auto height = candidateLayouts_[i].comment.fontHeight()) {
                commentFontHeight = height;
            }
            if (candidateLayouts_[i].label.characterCount()) {
                labelW = candidateLayouts_[i].label.width();
            }
            labelH = labelFontHeight *
                     std::max(1, candidateLayouts_[i].label.size());
            if (candidateLayouts_[i].text.characterCount()) {
                candidateW = candidateLayouts_[i].text.width();
            }
            candidateH =
                fontHeight * std::max(1, candidateLayouts_[i].text.size());
            if (candidateLayouts_[i].comment.characterCount()) {
                commentW = candidateLayouts_[i].comment.width();
            }
            commentH = commentFontHeight *
                       std::max(1, candidateLayouts_[i].comment.size());
            auto &candidate = candidateNodes_[i];

            YGNodeStyleSetFlexDirection(candidate.inner.get(),
                                        YGFlexDirectionRow);
            YGNodeStyleSetAlignSelf(candidate.text.get(), YGAlignCenter);
            YGNodeStyleSetAlignSelf(candidate.label.get(), YGAlignCenter);
            YGNodeStyleSetAlignSelf(candidate.comment.get(), YGAlignCenter);
            YGNodeStyleSetWidth(candidate.label.get(), labelW);
            YGNodeStyleSetHeight(candidate.label.get(), labelH);
            YGNodeStyleSetWidth(candidate.text.get(), candidateW);
            YGNodeStyleSetHeight(candidate.text.get(), candidateH);
            YGNodeStyleSetWidth(candidate.comment.get(), commentW);
            YGNodeStyleSetHeight(candidate.comment.get(), commentH);

            YGNodeStyleSetMargin(candidate.inner.get(), YGEdgeLeft,
                                 *textMargin.marginLeft);
            YGNodeStyleSetMargin(candidate.inner.get(), YGEdgeRight,
                                 *textMargin.marginRight + fontHeight);
            YGNodeStyleSetMargin(candidate.inner.get(), YGEdgeTop,
                                 *textMargin.marginTop);
            YGNodeStyleSetMargin(candidate.inner.get(), YGEdgeBottom,
                                 *textMargin.marginBottom);

            YGNodeInsertChild(candidatesNode_.get(), candidate.self.get(), i);
            YGNodeInsertChild(candidate.self.get(), candidate.inner.get(), 0);
            YGNodeInsertChild(candidate.inner.get(), candidate.label.get(), 0);
            YGNodeInsertChild(candidate.inner.get(), candidate.text.get(), 1);
            YGNodeInsertChild(candidate.inner.get(), candidate.comment.get(),
                              2);
        }
    }
    YGNodeStyleSetDisplay(buttonNode_.get(), YGDisplayNone);
    if (nCandidates_) {
        YGNodeStyleSetDisplay(buttonNode_.get(), YGDisplayFlex);
        YGNodeStyleSetWidth(buttonNode_.get(), 66);
    }

    YGNodeStyleSetDisplay(toolBarNode_.get(),
                          showToolBar_ ? YGDisplayFlex : YGDisplayNone);
    if (showToolBar_) {
        YGNodeStyleSetHeight(toolBarNode_.get(), 40);
        YGNodeStyleSetWidth(toolBarNode_.get(), 134);
        YGNodeStyleSetMargin(toolBarNode_.get(), YGEdgeTop, 4);
    }

    YGNodeStyleSetDisplay(emojiPanelNode_.get(),
                          showEmojiPanel_ ? YGDisplayFlex : YGDisplayNone);
    if (showEmojiPanel_) {
        YGNodeStyleSetWidth(emojiPanelNode_.get(), 360);
        YGNodeStyleSetHeight(emojiPanelNode_.get(), 74);
        YGNodeStyleSetMargin(emojiPanelNode_.get(), YGEdgeTop, 4);
    }

    // Calculate layout
    YGNodeCalculateLayout(rootNode_.get(), YGUndefined, YGUndefined,
                          YGDirectionLTR);
}

void InputWindow::renderYogaNode(cairo_t *cr, YGNodeRef node) {
    if (!node) {
        return;
    }

    // Save current cairo state
    cairo_save(cr);

    // Get node layout info
    float left = YGNodeLayoutGetLeft(node);
    float top = YGNodeLayoutGetTop(node);
    float width = YGNodeLayoutGetWidth(node);
    float height = YGNodeLayoutGetHeight(node);

    // Convert to integer pixels for better rendering
    int nodeX = static_cast<int>(left);
    int nodeY = static_cast<int>(top);
    int nodeWidth = static_cast<int>(width);
    int nodeHeight = static_cast<int>(height);

    // Move to node position
    cairo_translate(cr, nodeX, nodeY);

    // Draw border for debugging with different colors for different node types
    Color color;
    if (node == rootNode_.get()) {
        color = Color(0, 0, 255, 128); // Blue for root
    } else if (node == upperNode_.get() || node == upperTextNode_.get()) {
        color = Color(0, 255, 0, 128); // Green for upper
    } else if (node == lowerNode_.get() || node == auxDownNode_.get()) {
        color = Color(255, 255, 0, 128); // Yellow for lower
    } else if (node == candidatesNode_.get()) {
        color = Color(255, 0, 255, 128); // Magenta for candidates
    } else {
        color = Color(255, 0, 0, 76); // Red for individual candidates
    }
    cairoSetSourceColor(cr, color);

    cairo_rectangle(cr, 0, 0, nodeWidth, nodeHeight);
    cairo_stroke(cr);

    // Recursively render children
    uint32_t childCount = YGNodeGetChildCount(node);
    for (uint32_t i = 0; i < childCount; i++) {
        YGNodeRef child = YGNodeGetChild(node, i);
        renderYogaNode(cr, child);
    }

    // Restore cairo state
    cairo_restore(cr);
}

} // namespace fcitx::classicui
