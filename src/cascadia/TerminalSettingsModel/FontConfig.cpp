// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "FontConfig.h"
#include "FontConfig.g.cpp"

#include "TerminalSettingsSerializationHelpers.h"
#include "JsonUtils.h"

using namespace Microsoft::Terminal::Settings::Model;
using namespace winrt::Microsoft::Terminal::Settings::Model::implementation;

static constexpr std::string_view FontInfoKey{ "font" };
static constexpr std::string_view LegacyFontFaceKey{ "fontFace" };
static constexpr std::string_view LegacyFontSizeKey{ "fontSize" };
static constexpr std::string_view LegacyFontWeightKey{ "fontWeight" };

winrt::Microsoft::Terminal::Settings::Model::implementation::FontConfig::FontConfig(winrt::weak_ref<Profile> sourceProfile) :
    _sourceProfile(std::move(sourceProfile))
{
}

winrt::com_ptr<FontConfig> FontConfig::CopyFontInfo(const FontConfig* source, winrt::weak_ref<Profile> sourceProfile)
{
    auto fontInfo{ winrt::make_self<FontConfig>(std::move(sourceProfile)) };

#define FONT_SETTINGS_COPY(type, name, jsonKey, ...) \
    fontInfo->_##name = source->_##name;
    MTSM_FONT_SETTINGS(FONT_SETTINGS_COPY)
#undef FONT_SETTINGS_COPY

    // We cannot simply copy the font axes and features with `fontInfo->_FontAxes = source->_FontAxes;`
    // since that'll just create a reference; we have to manually copy the values.
    static constexpr auto cloneFontMap = [](const IFontFeatureMap& map) {
        std::map<winrt::hstring, float> fontAxes;
        for (const auto& [k, v] : map)
        {
            fontAxes.emplace(k, v);
        }
        return winrt::single_threaded_map(std::move(fontAxes));
    };
    if (source->_FontAxes)
    {
        fontInfo->_FontAxes = cloneFontMap(*source->_FontAxes);
    }
    if (source->_FontFeatures)
    {
        fontInfo->_FontFeatures = cloneFontMap(*source->_FontFeatures);
    }

    return fontInfo;
}

Json::Value FontConfig::ToJson() const
{
    Json::Value json{ Json::ValueType::objectValue };

#define FONT_SETTINGS_TO_JSON(type, name, jsonKey, ...) \
    JsonUtils::SetValueForKey(json, jsonKey, _##name);
    MTSM_FONT_SETTINGS(FONT_SETTINGS_TO_JSON)
#undef FONT_SETTINGS_TO_JSON

    return json;
}

// Method Description:
// - Layer values from the given json object on top of the existing properties
//   of this object. For any keys we're expecting to be able to parse in the
//   given object, we'll parse them and replace our settings with values from
//   the new json object. Properties that _aren't_ in the json object will _not_
//   be replaced.
// - Optional values that are set to `null` in the json object
//   will be set to nullopt.
// - This is similar to Profile::LayerJson but for FontConfig
// Arguments:
// - json: an object which should be a partial serialization of a FontConfig object.
void FontConfig::LayerJson(const Json::Value& json)
{
    // Legacy users may not have a font object defined in their profile,
    // so check for that before we decide how to parse this
    if (json.isMember(JsonKey(FontInfoKey)))
    {
        // A font object is defined, use that
        const auto fontInfoJson = json[JsonKey(FontInfoKey)];
#define FONT_SETTINGS_LAYER_JSON(type, name, jsonKey, ...)     \
    JsonUtils::GetValueForKey(fontInfoJson, jsonKey, _##name); \
    _logSettingValIfSet(fontInfoJson, jsonKey);

        MTSM_FONT_SETTINGS(FONT_SETTINGS_LAYER_JSON)
#undef FONT_SETTINGS_LAYER_JSON
    }
    else
    {
        // No font object is defined
        JsonUtils::GetValueForKey(json, LegacyFontFaceKey, _FontFace);
        _logSettingValIfSet(json, LegacyFontFaceKey);

        JsonUtils::GetValueForKey(json, LegacyFontSizeKey, _FontSize);
        _logSettingValIfSet(json, LegacyFontSizeKey);

        JsonUtils::GetValueForKey(json, LegacyFontWeightKey, _FontWeight);
        _logSettingValIfSet(json, LegacyFontWeightKey);
    }
}

winrt::Microsoft::Terminal::Settings::Model::Profile FontConfig::SourceProfile()
{
    return _sourceProfile.get();


//void FontConfig::_logSettingSet(std::string_view setting, winrt::Windows::Foundation::Collections::IMap<winrt::hstring, float>& value)
//{
//    OutputDebugStringA(setting.data());
//    OutputDebugStringA(" - ");
//
//    for (const auto& [mapKey, mapVal] : value)
//    {
//        std::string key{ setting };
//        key.append("." + til::u16u8(mapKey.c_str()));
//
//        _changeLog.insert_or_assign(key, std::to_string(mapVal));
//    }
//}

void FontConfig::_logSettingSet(std::string_view setting)
{
    OutputDebugStringA(setting.data());
    OutputDebugStringA(" - ");
    std::wstring val{ _convertVal(value).c_str() };
    _changeLog.insert_or_assign(setting, std::wstring_view{ val });
}

void FontConfig::_logSettingValIfSet(const Json::Value& json, std::string_view setting)
{
    if (auto found{ json.find(&*setting.cbegin(), (&*setting.cbegin()) + setting.size()) })
    {
        if (setting == "axes" || setting == "features")
        {
            for (auto it = json.begin(), end = json.end(); it != end; ++it)
            {
                std::string key{ setting };
                key.append("." + it.name());

                _changeLog.insert_or_assign(key, std::string_view{ it->asString() });
            }
        }
        else
        {
            _changeLog.insert_or_assign(setting, std::string_view{ found->asString() });
        }
    }
}

void FontConfig::LogSettingChanges(std::vector<std::pair<std::string_view, std::string_view>>& changes) const
{
    for (const auto& [key, val] : _changeLog)
    {
        std::string maskedKey{ "font." };
        maskedKey.append(key);
        changes.emplace_back(std::make_pair(maskedKey, val));
    }
}
