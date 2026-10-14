#include "document.h"

#include "container.h"   // normaliseVersion

#include <algorithm>
#include <set>

namespace xerum::model
{

std::string_view toString (Severity severity)
{
    switch (severity)
    {
        case Severity::Info:   return "info";
        case Severity::Medium: return "medium";
        case Severity::High:   return "high";
    }
    return "info";
}

std::size_t ImportReport::count (Severity severity) const
{
    return static_cast<std::size_t> (std::count_if (entries.begin(), entries.end(),
                                                    [severity] (const ReportEntry& e)
                                                    { return e.severity == severity; }));
}

Severity ImportReport::worst() const
{
    Severity result = Severity::Info;

    for (const auto& entry : entries)
        if (entry.severity == Severity::High)
            return Severity::High;
        else if (entry.severity == Severity::Medium)
            result = Severity::Medium;

    return result;
}

const SupportCatalogue& SupportCatalogue::v1()
{
    // Ticket 12's cut, by family rather than by class. Everything here is a claim about what xerum can
    // reproduce; anything a preset asks for that is not listed becomes a report entry rather than a
    // silent substitution.
    static const SupportCatalogue catalogue = []
    {
        SupportCatalogue c;

        c.schemaVersions = { "3.0", "4.0", "5.0", "6.0" };

        c.engines = { "WTOsc", "SubOsc", "NoiseOsc" };
        c.deferredEngines = { "GranularOsc", "SpectralOsc", "MultiSampleOsc", "SampleOsc" };

        // The shipped warp families, matched by mode-name prefix: phase distortion, FM/PM, distortion,
        // bend and asymmetry, sync/PWM, flip, quantize, ring and amplitude modulation, filter-as-warp.
        //
        // This list and `dsp::warpModeFromName` are two statements of one fact, so a test holds them
        // together over the reference's whole warp vocabulary (`warp_coverage_test.cpp`). They are not
        // one statement because the model layer deliberately links no DSP: see its CMakeLists for why.
        // `kFM` rather than `kFM_`: the through-zero and exponential variants are `kFMX_*` and `kFMP_*`,
        // and matching only the underscored spelling would report all six of them as unshipped while the
        // engine happily implements every one. The coverage test exists because exactly this happened.
        c.warpPrefixes = { "kPD_", "kSelfPD", "kFM", "kDist", "kBend", "kASYM", "kSync", "kPWM",
                           "kFlip", "kQuantize", "kRM_", "kAM_", "kFilterL", "kFilterH" };

        // PROVISIONAL. Ticket 26 owns the real model table; these are the family prefixes behind the
        // ~90% of corpus usage ticket 12 measured.
        c.filterModels = { "MgL", "Ladder", "DirtyMg", "LBH", "LNH", "LPH", "L",
                           "H", "HP", "BP", "BN", "BandReject", "PP",
                           "Comb", "Diffuser", "Flange", "Phase", "PZ" };

        c.effects = { "FXEQ", "FXComp", "FXReverb", "FXDelay", "FXDistortion", "FXFilter",
                      "FXConv", "FXUtils", "FXChorus", "FXHyperD", "FXBode", "FXPhaser",
                      "FXFlanger", "FXSplit", "FXSplit3", "FXSplitMS" };

        // Empty, and that is a correction rather than an oversight. Ticket 12 cut "the 14 of 16 classes
        // that cover 99%" from a list whose names were **guessed** — `FXConvolve`, `FXHyper`,
        // `FXUtility` do not exist, and the finding that three classes went unused in the factory bank
        // was an artefact of searching for names that were never real. The corpus says otherwise: all
        // **sixteen** real classes appear, the rarest being `FXSplitMS` at 15 uses and `FXSplit3` at 19.
        // Re-deriving the effect cut from real usage is ticket 12's follow-up; until then nothing is
        // deferred, because deferring something in use would be a silent drop.
        c.deferredEffects = {};

        return c;
    }();

    return catalogue;
}

namespace
{

bool startsWithAny (std::string_view text, const std::vector<std::string>& prefixes)
{
    for (const auto& prefix : prefixes)
        if (text.starts_with (prefix))
            return true;

    return false;
}

bool contains (const std::vector<std::string>& values, std::string_view wanted)
{
    return std::find (values.begin(), values.end(), wanted) != values.end();
}

bool looksLikeAssetReference (std::string_view key)
{
    return key.find ("relativePath") != std::string_view::npos || key.ends_with ("Path");
}

/// Path segments that are instances get normalised by the schema, so this can build concrete paths and
/// let `Schema::normalise` do the `{N}` work.
struct Walker
{
    const Schema& schema;
    const SupportCatalogue& catalogue;
    PresetDocument& document;
    ImportReport& report;
    std::set<std::string> alreadyReported;

    void note (Severity severity, std::string modulePath, std::string item, std::string reason)
    {
        // One entry per distinct complaint. A parameter missing from every LFO should read as one fact,
        // not sixty.
        const auto key = std::to_string (static_cast<int> (severity)) + "|" + item;

        if (! alreadyReported.insert (key).second)
            return;

        report.entries.push_back ({ severity, std::move (modulePath), std::move (item), std::move (reason) });
    }

    /// Which engine a slot selects. Slots 0–2 nest every engine and mark the chosen one with a real
    /// parameter map; the others carry the "default" sentinel. Slots 3 and 4 are fixed single-engine
    /// slots (ticket 05).
    std::string selectedEngine (const Value& slot) const
    {
        for (const auto& [key, value] : slot.entries)
        {
            if (! value.isMap())
                continue;

            const auto* params = value.find ("plainParams");

            if (params != nullptr && params->isMap())
            {
                const auto stem = key.substr (0, key.find_first_of ("0123456789"));
                return stem;
            }
        }

        return {};
    }

    void walkValue (const std::string& path, const Value& value, int depth)
    {
        if (depth > 24)
            return;

        if (value.isMap())
        {
            for (const auto& [key, child] : value.entries)
                walkValue (path + "/" + key, child, depth + 1);

            return;
        }

        if (value.isArray())
        {
            // The real index goes into the key. A template path would collapse every effect slot and
            // every curve point into one entry, losing all but the first — the schema is addressed by
            // template, but the document is addressed by instance.
            for (std::size_t i = 0; i < value.items.size(); ++i)
                walkValue (path + "[" + std::to_string (i) + "]", value.items[i], depth + 1);

            return;
        }

        // The "default" sentinel: a module whose parameters are all at their schema defaults. It is a
        // statement about every parameter in the module at once, not a value, and it sits in the path
        // where a parameter map would be. Reporting it as an unknown parameter would fill the report
        // with one bogus entry per unselected engine in every preset.
        if (value.isText() && value.text == "default" && path.ends_with ("/plainParams"))
            return;

        const auto leafKey = path.substr (path.find_last_of ('/') + 1);

        if (value.isText() && looksLikeAssetReference (leafKey))
            document.assetReferences.push_back (value.text);

        const auto normalised = schema.normalise (path);

        if (const auto* leaf = schema.describe (normalised); leaf == nullptr)
        {
            const auto module = normalised.substr (0, normalised.find ('/'));
            note (Severity::Medium, module, normalised,
                  "parameter is not in the schema — we did not interpret it");
            return;
        }

        // Keyed by the CONCRETE path. Keying by the schema's template path would make `Env0`'s attack
        // and `Env1`'s attack the same key, so `emplace` would keep the first and silently drop the
        // rest — a preset importing with only its first instance's values. The template path is for
        // looking things up, not for identity.
        document.parameters.emplace (path, value);
    }

    void walkModule (const std::string& name, const Value& module, int indexOrMinusOne)
    {
        bool selectedEngineChecked = false;

        for (const auto& [key, value] : module.entries)
        {
            if (key == "plainParams")
            {
                if (value.isText())
                    continue;   // the "default" sentinel: every parameter in this module is at its default

                if (value.isMap())
                    for (const auto& [param, paramValue] : value.entries)
                        walkValue (name + "/plainParams/" + param, paramValue, 0);

                continue;
            }

            walkValue (name + "/" + key, value, 0);
        }

        // Engines. Slots 3 and 4 are fixed, so only the first three choose.
        if (name.starts_with ("Oscillator") && indexOrMinusOne >= 0 && indexOrMinusOne <= 2)
        {
            const auto engine = selectedEngine (module);
            selectedEngineChecked = true;

            if (! engine.empty())
            {
                if (contains (catalogue.deferredEngines, engine))
                    note (Severity::High, name, engine,
                          "xerum cannot synthesise this engine yet — the slot will be silent");
                else if (! contains (catalogue.engines, engine))
                    note (Severity::High, name, engine, "engine not recognised at all");
            }
        }

        (void) selectedEngineChecked;
    }

    void run (const Value& payload)
    {
        if (! payload.isMap())
        {
            note (Severity::Medium, "", "payload", "payload is not a module map");
            return;
        }

        for (const auto& [name, module] : payload.entries)
        {
            if (! module.isMap())
                continue;

            // An index suffix, if there is one, tells slots from singletons.
            std::size_t digitsAt = name.size();

            while (digitsAt > 0 && std::isdigit (static_cast<unsigned char> (name[digitsAt - 1])) != 0)
                --digitsAt;

            const int index = digitsAt < name.size() ? std::stoi (name.substr (digitsAt)) : -1;

            if (! schema.recognisesModule (name) && ! schema.recognisesModule (name.substr (0, digitsAt) + "0"))
                note (Severity::Medium, name, name, "module is not in the schema — we did not interpret it");

            walkModule (name, module, index);

            // Effects live inside racks, and each rack entry names its class in a keyed object.
            if (name.starts_with ("FXRack"))
                checkEffects (name, module);

            // A warp mode or filter family we do not ship, reported by name so a user can see which.
            checkWarpsAndFilters (name, module);
        }
    }

    void checkEffects (const std::string& rackName, const Value& module)
    {
        const auto* fx = module.find ("FX");

        if (fx == nullptr || ! fx->isArray())
            return;

        for (const auto& entry : fx->items)
        {
            if (! entry.isMap())
                continue;

            for (const auto& [key, value] : entry.entries)
            {
                if (! value.isMap() || key == "type" || key == "flex" || key.starts_with ("kUI"))
                    continue;

                const auto& className = key;

                if (contains (catalogue.deferredEffects, className))
                    note (Severity::High, rackName, className, "effect is not shipped in v1");
                else if (! contains (catalogue.effects, className))
                    note (Severity::Medium, rackName, className, "unrecognised effect class");
            }
        }
    }

    void checkWarpsAndFilters (const std::string& name, const Value& module)
    {
        for (const auto& [key, value] : module.entries)
        {
            if (! value.isMap() || key == "plainParams")
                continue;

            const auto* params = value.find ("plainParams");

            if (params == nullptr || ! params->isMap())
                continue;

            if (const auto* warp = params->find ("kParamWarpMenu"); warp != nullptr && warp->isText())
                if (! startsWithAny (warp->text, catalogue.warpPrefixes))
                    note (Severity::High, name + "/" + key, warp->text, "warp family is not shipped in v1");

            if (const auto* warp2 = params->find ("kParamWarpMenu2"); warp2 != nullptr && warp2->isText())
                if (! startsWithAny (warp2->text, catalogue.warpPrefixes))
                    note (Severity::High, name + "/" + key, warp2->text, "warp family is not shipped in v1");

            if (name.starts_with ("VoiceFilter"))
                if (const auto* type = params->find ("kParamType"); type != nullptr && type->isText())
                    if (! startsWithAny (type->text, catalogue.filterModels))
                        note (Severity::High, name, type->text, "filter family is not shipped in v1");
        }
    }
};

} // namespace

LoadResult loadPreset (const Value& payload, std::string_view schemaVersion, const Schema& schema,
                       const SupportCatalogue& catalogue)
{
    LoadResult result;

    if (schemaVersion.empty())
    {
        // Not fatal on its own — the payload may still parse — but it must not pass unnoticed, because
        // the version decides which parameters are even supposed to exist.
        result.report.entries.push_back ({ Severity::Medium, "preset", "schema version",
                                           "the container carries no schema version" });
    }
    else if (! contains (catalogue.schemaVersions, schemaVersion))
    {
        // Parsed as far as the schema allows rather than refused: refusing makes xerum useless against a
        // future release, and silence would hide the gap.
        result.report.entries.push_back ({ Severity::Medium, "preset", std::string (schemaVersion),
                                           "schema version is not one of the ones we recognise — parsed "
                                           "as far as we could rather than refused" });
    }

    result.document.schemaVersion = std::string (schemaVersion);

    Walker walker { schema, catalogue, result.document, result.report, {} };
    walker.run (payload);

    result.ok = true;
    return result;
}

PresetDocument neverMerges (const PresetDocument&, const PresetDocument& replacement)
{
    return replacement;
}

} // namespace xerum::model
