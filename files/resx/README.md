# .NET culture snapshot

dotnet10-cultures.json contains858 name/displayName pairs, embedded by
resx-cultures.qrc. Generated from the local Mission Planner10
ResxTranslationService.Cultures with .NET10.0.11 and en-US current/UI culture,
plus GetCultureInfo aliases zh-CN, zh-TW, zh-HK, zh-MO, zh-SG, zh-CHS, zh-CHT.
Deduplicate names using OrdinalIgnoreCase; sort displayName with en-US
CurrentCultureIgnoreCase then name with OrdinalIgnoreCase, as MP10 does.

The aliases are accepted by .NET but missing from Linux ICU GetCultures. They
prevent45 Chinese-localized files in MP10 from being misclassified as neutral.
This data does not provide Qt application translations or a runtime dependency
on .NET. Regeneration provenance, measurements and limits are recorded in
docs/porting/RESX_TRANSLATION_EDITOR_PORT.md.
