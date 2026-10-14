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
        // Ticket 26's model table: the **exact** names this build implements, not prefixes.
        //
        // Prefixes were the obvious first move and they over-claim. `FlangeP` is a real type, and so is
        // `FlangePhase12HL6P`, which starts with it — so a prefix list claimed a filter the engine
        // deliberately does not build, and the report stayed quiet about it. Warp names need prefixes
        // because they carry a source suffix; filter names are a closed 83-name vocabulary with no
        // suffixes, so they are matched exactly and this list is the claim.
        //
        // The 17 it leaves out are individually authored circuits whose behaviour no name decomposes
        // (`RM`, `Reverb1`, `Wsp`, `Exp`, `ExpBPF`, `Scream*`, `Formant*`, `DJMixer`, `ADD_BASS`,
        // `ZDF_A`) plus the two hybrids whose second half is not modelled. The coverage test holds this
        // list and `dsp::filterSpecFromName` to each other over the whole vocabulary.
        c.filterModels = {
            "Allpasses", "B12", "B24", "BN12", "BP12",
            "BPN12", "BPN24", "BandReject", "Comb2", "CombH6N",
            "CombH6P", "CombHL6N", "CombHL6P", "CombL6N", "CombN",
            "CombP", "Combs", "Diffuser", "DirtyMg", "DistComb1BP",
            "DistComb1LP", "DistComb2BP", "DistComb2LP", "FlangeH6P", "FlangeHL6N",
            "FlangeHL6P", "FlangeL6P", "FlangeN", "FlangeP", "H12",
            "H18", "H24", "H6", "HB12", "HEQ12",
            "HN12", "HP12", "L12", "L18", "L24",
            "L6", "LB12", "LBH12", "LBH24", "LH12",
            "LN12", "LNH12", "LNH24", "LPH24", "LadderAcid",
            "LadderEMS", "LadderMg", "MgL18", "MgL24", "MgL6",
            "N12", "N24", "NN12", "PP12", "PZ_SVF",
            "Phase24N", "Phase24P", "Phase36N", "Phase36P", "Phase48N",
            "Phase48P",
        };

        c.effects = { "FXEQ", "FXComp", "FXReverb", "FXDelay", "FXDistortion", "FXFilter",
                      "FXConv", "FXUtils", "FXChorus", "FXHyperD", "FXBode", "FXPhaser",
                      "FXFlanger", "FXSplit" };

        // Ticket 29's cut, re-derived from measured usage: `.scratch/xerum/tools/corpus-usage.py`
        // over the 626 factory presets counts 4168 effect occurrences in 16 distinct classes, and
        // **99% of those occurrences is reached at exactly 14 classes**. The two outside that
        // threshold are these; `FXSplitMS` (15 uses) and `FXSplit3` (19) are the rarest of all.
        //
        // An earlier version of this list was built on names that do not exist — `FXConvolve`,
        // `FXHyper` and `FXUtility` are not real, so the "three classes unused in the factory bank"
        // it reported was an artefact of looking for those names. Every one of the sixteen real
        // classes appears in the corpus. `tests/model/fx_topology_test.cpp` holds this list and the
        // engine's own `dsp::fxClassIsShipped` to each other over the whole corpus.
        c.deferredEffects = { "FXSplit3", "FXSplitMS" };

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

template <std::size_t N>
bool containsValue (const std::string_view (&values)[N], std::string_view wanted)
{
    for (const auto& value : values)
        if (value == wanted)
            return true;

    return false;
}

// ---------------------------------------------------------------------------------------------------
// The modulation row.
//
// A row is the one place in the payload where a field is a *reference* into another part of the model:
// `destModuleParamName` names a parameter, `source` names a source, and `destModuleTypeString` names the
// class the parameter belongs to. That makes it the place where "the field has the right shape" and "the
// engine acts on the field" come apart, and three loads in one week came apart there: a voice filter
// type that was never checked at all (tickets 23/26), a subtree deeper than the walk's limit that
// returned without a word (ticket 36), and — the one this block fixes — **a destination parameter the
// parameter model cannot describe, which the walk accepted for being a string** (ticket 27's finding,
// ticket 39's fix).
//
// The rule the import contract states, and the form this takes, is: every field a row stores either
// resolves to something the engine acts on, or appears in the report.
// ---------------------------------------------------------------------------------------------------

/// The fields of a row the **engine** acts on — one entry per member of `dsp::ModSlot`
/// (`src/dsp/modulation.h`, the frozen contract):
///
///     source[]              -> ModSlot::source and ::auxSource, the reference's two-element pair
///     destModuleTypeString  -> ModSlot::destinationClass
///     destModuleID          -> ModSlot::destinationInstance
///     destModuleParamID     -> ModSlot::destinationParameter
///     destModuleParamName   -> ModSlot::destinationName
///     kParamAmount          -> ModSlot::amount      (percent, signed: the sign IS the polarity)
///     kParamBipolar         -> ModSlot::bipolar
///     kParamBypass          -> ModSlot::bypassed
///     kParamCurveIn         -> ModSlot::curveIn
///     kParamCurveOut        -> ModSlot::curveOut
///
/// Everything else a preset stores inside a row is a field the engine does not read, and the corpus says
/// that is not a corner: `flex` (the drawn transfer curve) carries points in 453 rows, and
/// `kParamMainCurveData` is stored by 423, `kParamOut` (the OUTPUT trim) by 156, `kParamAuxCurveData` by
/// 95, `kParamAuxInverted` by 52, the smoothing by up to 43, the delay by 7 and `kParamAuxCurve` by 3 —
/// each of them an ordinary schema leaf, each of them loading in silence.
///
/// **Two statements of one fact.** `tests/model/modulation_coverage_test.cpp` classifies every field of
/// every row in the factory bank from the document, independently of this table, and holds the result to
/// the report preset by preset; `tests/model/structural_fidelity_test.cpp` sweeps *every* field the
/// schema declares under `ModSlot{N}` and requires each to be either applied here or named in the
/// report. A field the engine grows, or one it stops acting on, therefore fails a test rather than
/// loading in silence — which is the whole point of fixing the third instance this way.
constexpr std::string_view kAppliedRowKeys[] = { "source", "destModuleTypeString", "destModuleID",
                                                 "destModuleParamID", "destModuleParamName" };

constexpr std::string_view kAppliedRowParamKeys[] = { "kParamAmount", "kParamBipolar", "kParamBypass",
                                                      "kParamCurveIn", "kParamCurveOut" };

/// The **severity** of both classes of entry this block adds is `medium`, and the argument is the
/// contract's own table (§3). Its `high` examples — an unsupported engine, warp, filter, effect or
/// asset, plus ticket 37's spectral-morph substitution — are all *sound-producing components or assets
/// on the audio path*: the things a preset's sound is made of. A row's fields shape a **control signal**
/// whose destination is still modulated: the row applies, at the authored source, polarity and depth,
/// and what is missing is a transfer curve, a trim, the smoothing, or a name for the destination. That
/// is "something was present that we did not interpret" — `medium`, whose own examples ("an unknown
/// module, parameter") are fields rather than components — and it is the severity the loader already
/// gives a parameter outside the schema.
struct RowFieldExplanation
{
    std::string_view field;   ///< relative to the row (`flex`) or to its `plainParams` (`kParamOut`)
    std::string_view why;
};

/// What we do instead, for the three row fields whose absence a user is most likely to hear. Every other
/// unapplied field gets the honest generic reason: the row contract has no place to put it.
constexpr RowFieldExplanation kRowFieldExplanations[] = {
    { "flex",
      "the reference drew the row's transfer curve and the engine evaluates the CRV and OUT knobs "
      "(`kParamCurveIn`/`kParamCurveOut`) in its place" },
    { "plainParams/kParamOut",
      "the row's output trim is dropped, so the row's depth is `kParamAmount` alone" },
    { "plainParams/kParamAuxInverted",
      "the row's aux invert is dropped, so the aux source scales the row un-inverted" },
};

/// The reason a report gives for a field of a row the engine does not apply.
std::string rowFieldReason (std::string_view field)
{
    const auto opening = "the row's field `" + std::string (field) + "` is stored and not applied: ";

    for (const auto& explained : kRowFieldExplanations)
        if (explained.field == field)
            return opening + std::string (explained.why);

    return opening + "the engine's row contract (`dsp::ModSlot`) has no field for it";
}

/// The parameter model's module path for a modulation destination class, with `{N}` for the instance.
///
/// This is the model-side half of the engine's `dsp::modDestinationClassKnown`, and it is a
/// correspondence rather than a guess in all three shapes: an FX destination names a class inside a
/// rack's `FX` array; `Arp` destinations name the arp's parameters, which the model stores under
/// `ArpClip{N}`; and an oscillator-engine destination names a leaf one level deeper, under the slot's
/// own engine module. Everything else is `<Class>{N}`.
///
/// `modulation_coverage_test.cpp` builds the same mapping from the other side and holds the two together
/// over the whole corpus: the report's item for an undescribed destination parameter is the leaf this
/// produces, so a mapping that drifted would fail the walk's own assertion rather than quietly
/// mis-resolving.
std::string schemaModuleForDestinationClass (std::string_view destinationClass)
{
    if (destinationClass.starts_with ("FX"))
        return "FXRack{N}/FX[]/" + std::string (destinationClass) + "/plainParams/";

    if (destinationClass == "Arp")
        return "ArpClip{N}/plainParams/";

    for (const auto* engine : { "WTOsc", "SampleOsc", "MultiSampleOsc", "GranularOsc", "SpectralOsc",
                                "NoiseOsc" })
        if (destinationClass == engine)
            return "Oscillator{N}/" + std::string (destinationClass) + "{N}/plainParams/";

    return std::string (destinationClass) + "{N}/plainParams/";
}

/// Whether a value holds anything at all.
///
/// `{}` and `[]` are what the reference writes for a container it has nothing to say about — the schema
/// types them `empty-map` and `empty-array` — so an empty `flex` curve means "nothing drawn", not "a
/// drawn curve we dropped". The corpus says the difference is real: 504 rows carry a `flex` array and 51
/// of those hold two empty curves, leaving 453 with something to lose.
bool holdsAnything (const Value& value)
{
    if (value.isMap())
        return std::any_of (value.entries.begin(), value.entries.end(),
                            [] (const auto& entry) { return holdsAnything (entry.second); });

    if (value.isArray())
        return std::any_of (value.items.begin(), value.items.end(),
                            [] (const Value& item) { return holdsAnything (item); });

    return value.type != Value::Type::Null;
}

/// Whether a row's `source` array names a source at all. `[0, 0]` is the reference's "no source".
bool namesSource (const Value* source)
{
    if (source == nullptr || ! source->isArray() || source->items.empty())
        return false;

    double first = 0.0;
    return source->items.front().numberAsDouble (first) && first != 0.0;
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

    /// `interpolateAfterLoad = 2` leaves, held until the walk is done. Whether the request is honoured
    /// depends on the slot carrying an embedded table, which the leaf itself cannot see — so it is
    /// decided in `finish()`, after the preset's shape is known. See that function.
    std::vector<std::string> morphRequests;

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
        {
            // Reported, not just refused. This was the last place in the loader that dropped something
            // without saying so — and the contract's rule is that the set of silent drops is empty, not
            // that it is small. Dormant for the factory bank (its deepest path is 8 levels), live for a
            // third-party preset that nests further.
            note (Severity::Medium, path.substr (0, path.find ('/')), path,
                  "nested deeper than 24 levels — we did not interpret this subtree");
            return;
        }

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

        // `WTOsc{N}/interpolateAfterLoad == 2` asks the engine to morph an **embedded** table after load,
        // and that is now in the signal path: the plugin expands the table once at bind
        // (`dsp::tableForInterpolateAfterLoad`) and the oscillator reads the morphed slots. So the request
        // is honoured, and complaining about it would turn a true statement into a false one.
        //
        // The field applies to an embedded table only — ticket 37 measured the reference ignoring it for a
        // table referenced by path — so a preset that sets it and inlines no table for the slot is asking
        // for something nothing can act on, and that stays reported. Which of the two a preset is cannot
        // be seen from this leaf, so the decision is deferred to `finish()`. Exactly one factory preset
        // sets the value (`KY - Delicacy`, oscillator A) and it carries its table inline.
        double afterLoad = 0.0;

        if (leafKey == "interpolateAfterLoad" && value.numberAsDouble (afterLoad) && afterLoad == 2.0)
            morphRequests.push_back (path);

        const auto normalised = schema.normalise (path);

        if (const auto* leaf = schema.describe (normalised); leaf == nullptr)
        {
            const auto module = normalised.substr (0, normalised.find ('/'));
            note (Severity::Medium, module, normalised,
                  "parameter is not in the schema — we did not interpret it");

            // **Reported and retained.** Reporting alone satisfies "nothing is silently dropped" for a
            // *load*, but not for a *write*: dropping it from the document means writing the preset back
            // omits a field the reference put there, which is losing data we were shown. A newer
            // reference's parameters round-trip as opaque values; we still say we did not interpret them.
            document.parameters.emplace (path, value);
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

            // A modulation row: the one module whose *fields* mean something elsewhere in the model, and
            // so the one module where the shape of a field is not the meaning of it. See
            // `checkModulationRow`.
            if (name.starts_with ("ModSlot"))
                checkModulationRow (name, module);
        }
    }

    /// Ticket 41: the `interpolateAfterLoad = 2` requests the engine has nothing to act on.
    ///
    /// The morph applies to an **embedded** table — the plugin expands it once at bind — so a slot that
    /// carries no `embeddedWTData` has nothing to morph, and the request is named. A slot that does carry
    /// one is honoured (the plugin expands it) and is deliberately silent. This is decided here rather
    /// than at the leaf because only the whole preset shows whether the table is inline.
    void finish()
    {
        for (const auto& path : morphRequests)
        {
            const auto module = path.substr (0, path.find_last_of ('/'));
            const auto embedded = module + "/embeddedWTData[";

            const auto first = document.parameters.lower_bound (embedded);
            const bool carried = first != document.parameters.end() && first->first.starts_with (embedded);

            if (carried)
                continue;

            note (Severity::High, path, "interpolateAfterLoad=2",
                  "the preset asks for spectral morphing after load, but carries no embedded table for the "
                  "slot — the morph applies to an embedded table and there is none here, so nothing acts "
                  "on the request");
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

    /// A filter's type, reported when we do not ship that family.
    void checkFilterType (const std::string& where, const Value& params)
    {
        const auto* type = params.find ("kParamType");

        if (type != nullptr && type->isText() && ! contains (catalogue.filterModels, type->text))
            note (Severity::High, where, type->text, "filter family is not shipped in v1");
    }

    void checkWarpsAndFilters (const std::string& name, const Value& module)
    {
        // **A voice filter keeps its parameters in its own `plainParams`**, unlike an oscillator, whose
        // engines are nested one level below it:
        //
        //     VoiceFilter0/plainParams/kParamType        ← the filter itself
        //     Oscillator0/WTOsc0/plainParams/kParamWarpMenu   ← nested
        //
        // So this has to look at the module's own parameters *as well as* its children's. The first
        // version only looked at children, with the filter test gated on the outer module being a voice
        // filter — which no nested module ever is. The result was that **no voice filter type was ever
        // checked**: seven types in use across the factory bank could be dropped without a report, in
        // the one place the contract says must never drop anything silently.
        if (const auto* own = module.find ("plainParams"); own != nullptr && own->isMap())
            if (name.starts_with ("VoiceFilter"))
                checkFilterType (name, *own);

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

            // The master filter runs identically to the voice filter but sits in a rack, under its own
            // keyed module, so its type is checked on the same terms.
            if (key == "FXFilter")
                checkFilterType (name + "/" + key, *params);
        }
    }

    /// A field of a modulation row the engine does not apply, reported once per field.
    ///
    /// Two silences are deliberate and each has a reason. A field holding an empty container is not a
    /// loss (`holdsAnything`: the reference writes `{}` for "nothing here"). A field the *schema* does
    /// not know is already reported by the generic walk as "parameter is not in the schema", and two
    /// entries for one fact is noise rather than thoroughness.
    void reportUnappliedRowField (const std::string& field, const Value& value)
    {
        if (! holdsAnything (value))
            return;

        const std::string templateRow = "ModSlot{N}";
        const auto templatePath = templateRow + "/" + field;

        // `flex` is an array in the schema (`ModSlot{N}/flex[]`) and a map at its leaves; every other
        // row field is a leaf, so either spelling is a field the model describes.
        if (schema.describe (templatePath) == nullptr && schema.describe (templatePath + "[]") == nullptr)
            return;

        note (Severity::Medium, templateRow, templatePath, rowFieldReason (field));
    }

    /// A modulation row, checked for **meaning** rather than for shape.
    ///
    /// Three checks, and the first is the ticket's whole subject:
    ///
    ///   1. **the destination parameter must be one the parameter model describes.**
    ///      `destModuleParamName` is a string leaf, so the walk stored it and said nothing, whatever it
    ///      said. 135 rows in the factory bank name a parameter the model cannot describe — 128 of them
    ///      `Global.kParamMasterTuning`, 7 in `RetriggerState` — and every one of them imported
    ///      "cleanly" while the matrix computed a value for a destination nothing can consume. Asking
    ///      whether the name *addresses a schema leaf* is the check that was missing; "is a string" is
    ///      not a resolution.
    ///   2. **every field the row stores must be one the engine acts on**, or it appears in the report.
    ///      This is the general rule, and it is here rather than in a test so that a fourth silent field
    ///      is not possible: the applied vocabulary is `kAppliedRowKeys`/`kAppliedRowParamKeys`, and
    ///      anything outside it is named.
    ///   3. **a row that names a source must address a destination.** The 6 rows in `SY - Tetris` kept
    ///      a source and an amount and lost their destination; that is a modulation going nowhere, and
    ///      an *absent* field has no shape to validate, which is why only a meaning-check can see it.
    ///
    /// The instance is deliberately not checked here. `destModuleID`'s range is the **engine's**
    /// contract (`dsp::modDestinationInstanceKnown`, which knows the reference's instance counts and the
    /// FX `rackIndex * 100 + slotIndex` encoding), not the parameter model's — the schema's `{N}` admits
    /// any index. This check is about the parameter the row names, and the coverage walk sees all 13,258
    /// rows resolve their address.
    void checkModulationRow (const std::string& name, const Value& module)
    {
        const std::string templateRow = "ModSlot{N}";
        const auto* source = module.find ("source");
        const auto* params = module.find ("plainParams");

        for (const auto& [key, value] : module.entries)
        {
            if (key == "plainParams")
            {
                if (params == nullptr || ! params->isMap())
                    continue;   // the "default" sentinel: the row stores no parameter at all

                for (const auto& [param, paramValue] : params->entries)
                    if (! containsValue (kAppliedRowParamKeys, param))
                        reportUnappliedRowField ("plainParams/" + param, paramValue);

                continue;
            }

            if (! containsValue (kAppliedRowKeys, key))
                reportUnappliedRowField (key, value);
        }

        // `source` is the reference's two-element `[source, auxSource]` pair; a third element is a source
        // the contract has no room for, and the corpus never stores one.
        if (source != nullptr && source->isArray() && source->items.size() > 2)
            note (Severity::Medium, templateRow, templateRow + "/source[]",
                  "the row stores more than two sources and the engine addresses the first two");

        const auto destinationClass = module.stringOr ("destModuleTypeString");
        const auto parameterName = module.stringOr ("destModuleParamName");

        if (destinationClass.empty() || destinationClass == "None")
        {
            // The reference leaves all 64 rows present in every preset, most of them with nothing in
            // them at all, so an empty row is not a dropped modulation and must not be a complaint.
            if (namesSource (source))
                note (Severity::Medium, templateRow, templateRow + "/destination",
                      "the row names a source and no destination — its modulation addresses nothing");

            return;
        }

        if (parameterName.empty())
        {
            note (Severity::Medium, templateRow, templateRow + "/destination",
                  "the row names no destination parameter — its modulation addresses nothing");
            return;
        }

        // The name is the identity, not the id: `destModuleParamID` is version-dependent — `Oscillator[10]`
        // is `kParamDetune` at schema 3.0 and `kParamStart` at 4.0 — so the id cannot be asked this
        // question at all (ticket 27's finding).
        const auto leaf = schemaModuleForDestinationClass (destinationClass) + parameterName;

        if (schema.describe (leaf) != nullptr)
            return;

        // Named, not counted: the item is the parameter's path in the model's own vocabulary, which is
        // how a user finds which of their preset's modulations goes nowhere.
        note (Severity::Medium, templateRow, leaf,
              "the destination parameter `" + leaf + "` is not in the parameter model — the row's "
              "address resolves and its modulation is applied to nothing");
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

    Walker walker { schema, catalogue, result.document, result.report, {}, {} };
    walker.run (payload);
    walker.finish();

    result.ok = true;
    return result;
}

PresetDocument neverMerges (const PresetDocument&, const PresetDocument& replacement)
{
    return replacement;
}

} // namespace xerum::model
