// What a `PresetDocument` can and cannot represent — ticket 36's enumeration, as tests.
//
// The ticket asked for every structural fact the model cannot hold to be either represented or listed
// with the reason it is accepted. These are the four candidates it named, plus the one this work found:

#include <catch2/catch_test_macros.hpp>

#include "cbor.h"
#include "container.h"
#include "document.h"
#include "schema.h"
#include "write.h"

#include "generated/schema_data.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

using namespace xerum::model;

namespace
{

Value text (std::string value)
{
    Value v;
    v.type = Value::Type::String;
    v.text = std::move (value);
    return v;
}

Value number (double value)
{
    Value v;
    v.type = Value::Type::Float;
    v.real = value;
    return v;
}

Value integer (std::int64_t value)
{
    Value v;
    v.type = Value::Type::Int;
    v.integer = value;
    return v;
}

Value arrayOf (std::vector<Value> items)
{
    Value v;
    v.type = Value::Type::Array;
    v.items = std::move (items);
    return v;
}

Value object (std::initializer_list<std::pair<std::string, Value>> members)
{
    Value v;
    v.type = Value::Type::Map;
    for (const auto& [key, value] : members)
        v.entries.emplace_back (key, value);
    return v;
}

/// Replace a member or add it. Building a row by overriding one field is the shape every case below needs,
/// and a duplicate member would leave the *first* value in the document (`Value::find` returns the first),
/// which would quietly test the wrong field.
void putMember (Value& map, const std::string& key, Value value)
{
    for (auto& [existing, member] : map.entries)
        if (existing == key)
        {
            member = std::move (value);
            return;
        }

    map.entries.emplace_back (key, std::move (value));
}

// ---------------------------------------------------------------------------------------------------
// Ticket 39 — the modulation row.
// ---------------------------------------------------------------------------------------------------

/// The fields of a modulation row the **engine** acts on: one entry per member of `dsp::ModSlot`
/// (`src/dsp/modulation.h`). This is the second statement of the fact `src/model/document.cpp` states in
/// `kAppliedRowKeys`/`kAppliedRowParamKeys` — and the sweep below holds the two together over *every*
/// field the schema declares for a row, not only the fields the factory bank happens to use.
constexpr std::string_view kEngineRowFields[] = {
    "source",                // ModSlot::source and ::auxSource
    "destModuleTypeString",  // ModSlot::destinationClass
    "destModuleID",          // ModSlot::destinationInstance
    "destModuleParamID",     // ModSlot::destinationParameter
    "destModuleParamName",   // ModSlot::destinationName
    "plainParams/kParamAmount",
    "plainParams/kParamBipolar",
    "plainParams/kParamBypass",
    "plainParams/kParamCurveIn",
    "plainParams/kParamCurveOut",
};

bool engineActsOn (std::string_view field)
{
    for (const auto applied : kEngineRowFields)
        if (applied == field)
            return true;

    return false;
}

/// Which field of its row a schema leaf belongs to: the subtree's root, since `flex[]/xVals[]` is the
/// field `flex` however many points it holds, and a plain parameter is named under `plainParams`.
std::string rowFieldKey (std::string_view relative)
{
    if (relative.starts_with ("plainParams/"))
    {
        const auto parameter = relative.substr (std::string_view ("plainParams/").size());
        return "plainParams/" + std::string (parameter.substr (0, parameter.find ('/')));
    }

    return std::string (relative.substr (0, relative.find ('[')));
}

/// A value of the kind the schema records for a field. The loader asks whether a field is one the engine
/// acts on, never what its value is, so these only have to be storable.
Value valueForKind (std::string_view kind)
{
    if (kind == "string")
        return text ("kSomething");

    if (kind == "bool")
    {
        Value v;
        v.type = Value::Type::Bool;
        v.boolean = true;
        return v;
    }

    if (kind == "empty-array")
        return arrayOf ({});

    if (kind == "empty-map")
        return object ({});

    return kind == "int" ? integer (1) : number (0.5);
}

/// A drawn transfer curve, as `flex` holds it: one curve with points. An *empty* curve is deliberately
/// not a loss — the reference writes `{}` for "nothing drawn", and 51 of the corpus's 504 `flex` arrays
/// hold two empty curves — so the sweep has to supply a real one.
Value drawnCurve()
{
    return arrayOf ({ object ({ { "numPoints", integer (2) },
                                { "xVals", arrayOf ({ number (0.0), number (1.0) }) },
                                { "yVals", arrayOf ({ number (0.0), number (1.0) }) },
                                { "curveVals", arrayOf ({ number (0.0), number (1.0) }) } }) });
}

/// The value to store for a field under test. Everything the engine acts on gets a value its contract can
/// use — a real source pair, a destination the parameter model describes — so that a complaint about the
/// row is attributable to the field under test and not to the row around it.
Value valueForField (const std::string& field, std::string_view kind)
{
    if (field == "source")
        return arrayOf ({ number (1.0), number (0.0) });

    if (field == "destModuleTypeString")
        return text ("LFO");

    if (field == "destModuleParamName")
        return text ("kParamRate");

    if (field == "flex")
        return drawnCurve();

    return valueForKind (kind);
}

/// A well-formed row — a source pair, a destination the model describes, an amount — with one field
/// overridden.
Value modulationRowWith (const std::string& field, Value value)
{
    Value row = object ({ { "source", arrayOf ({ number (1.0), number (0.0) }) },
                          { "destModuleTypeString", text ("LFO") },
                          { "destModuleID", number (0.0) },
                          { "destModuleParamID", number (0.0) },
                          { "destModuleParamName", text ("kParamRate") },
                          { "plainParams", object ({ { "kParamAmount", number (50.0) } }) } });

    const std::string plainAt = "plainParams/";

    if (field.starts_with (plainAt))
    {
        for (auto& [key, member] : row.entries)
            if (key == "plainParams")
                putMember (member, field.substr (plainAt.size()), std::move (value));
    }
    else
    {
        putMember (row, field, std::move (value));
    }

    return row;
}

/// A row addressing the given class and parameter, with nothing else on it.
Value rowTo (std::string destinationClass, std::string parameter)
{
    return object ({ { "source", arrayOf ({ number (1.0), number (0.0) }) },
                     { "destModuleTypeString", text (std::move (destinationClass)) },
                     { "destModuleID", number (0.0) },
                     { "destModuleParamID", number (1.0) },
                     { "destModuleParamName", text (std::move (parameter)) },
                     { "plainParams", object ({ { "kParamAmount", number (50.0) } }) } });
}

/// The import-report entry whose item is exactly this, or nullptr. Items are the vocabulary the loader
/// promises: `ModSlot{N}/<field>` for a field or a row's own state, and the parameter model's leaf path
/// for a destination parameter the model cannot describe.
const ReportEntry* entryForItem (const LoadResult& result, std::string_view item)
{
    for (const auto& entry : result.report.entries)
        if (entry.item == item)
            return &entry;

    return nullptr;
}

LoadResult load (const Value& payload)
{
    return loadPreset (payload, "4.0", Schema::instance(), SupportCatalogue::v1());
}

bool wasReported (const LoadResult& result, std::string_view reasonFragment)
{
    for (const auto& entry : result.report.entries)
        if (entry.reason.find (reasonFragment) != std::string::npos)
            return true;

    return false;
}

/// Reload a document through the writer, which is the only way to see what a write would lose.
LoadResult roundTrip (const PresetDocument& document)
{
    const auto written = writeContainer (document, {});
    REQUIRE (written.ok);

    const auto container = readContainer (written.bytes);
    REQUIRE (container.ok);

    const auto payload = decompress (container.payload, container.codec, container.declaredSize);
    REQUIRE (payload.ok);

    const auto decoded = decodeCbor (payload.bytes);
    REQUIRE (decoded.ok);

    const auto result = loadPreset (decoded.value, container.schemaVersion, Schema::instance(),
                                    SupportCatalogue::v1());
    REQUIRE (result.ok);
    return result;
}

} // namespace

TEST_CASE ("a parameter outside the schema is reported and retained, so a write cannot lose it",
           "[structural]")
{
    // The one real loss this ticket's enumeration found. Reporting it satisfies "nothing is silently
    // dropped" for a load — but dropping it from the document means the preset writes back missing a
    // field the reference put there, which is losing data we were shown. Both, then: named in the
    // report *and* kept as an opaque value.
    const auto result = load (object ({ { "Oscillator0",
                                         object ({ { "WTOsc0",
                                                     object ({ { "plainParams",
                                                                 object ({ { "kParamVolume", number (0.5) },
                                                                           { "kParamFromTheFuture",
                                                                             number (0.25) } }) } }) } }) } }));

    REQUIRE (result.ok);

    CHECK (wasReported (result, "parameter is not in the schema"));

    const auto found = result.document.parameters.find ("Oscillator0/WTOsc0/plainParams/kParamFromTheFuture");
    REQUIRE (found != result.document.parameters.end());
    CHECK (found->second.real == 0.25);

    // And it survives the round trip, which is the claim that matters.
    const auto reread = roundTrip (result.document);

    REQUIRE (reread.document.parameters.find ("Oscillator0/WTOsc0/plainParams/kParamFromTheFuture")
             != reread.document.parameters.end());
    CHECK (reread.document.parameters.at ("Oscillator0/WTOsc0/plainParams/kParamFromTheFuture").real
           == 0.25);
}

TEST_CASE ("the default sentinel and an empty override map are equivalent, and both are accepted",
           "[structural]")
{
    // `plainParams` is either a sparse override map or the string `"default"`. A document cannot tell
    // them apart — both store nothing — and that is *accepted* rather than fixed, because they mean the
    // same thing: every parameter in the module sits at its schema default. The difference is a spelling,
    // not a state.
    const auto asSentinel = load (object ({ { "ModSlot0", object ({ { "plainParams", text ("default") } }) } }));
    const auto asEmpty = load (object ({ { "ModSlot0", object ({ { "plainParams", object ({}) } }) } }));

    REQUIRE (asSentinel.ok);
    REQUIRE (asEmpty.ok);

    CHECK (asSentinel.document.parameters.empty());
    CHECK (asEmpty.document.parameters.empty());

    // Neither is a complaint: the sentinel sits where a parameter map belongs, and reporting it would
    // fill the report with one bogus entry per unselected engine in every preset.
    CHECK_FALSE (wasReported (asSentinel, "parameter is not in the schema"));
}

TEST_CASE ("module presence with no parameters is not represented, and does not need to be", "[structural]")
{
    // A module can exist in the file with nothing in it. Absent and present-but-empty both mean "all
    // defaults", so the write emits nothing and the reference reads the same state. Accepted.
    const auto result = load (object ({ { "Env0", object ({ { "plainParams", text ("default") } }) },
                                        { "Env1", object ({}) } }));

    REQUIRE (result.ok);
    CHECK (result.document.parameters.empty());
    CHECK (result.report.worst() != Severity::High);
}

TEST_CASE ("a subtree nested deeper than the loader's limit is reported, not dropped in silence",
           "[structural]")
{
    // The loader refuses beyond 24 levels. It used to return without a word — the only silent drop left
    // in the whole loader, against a contract whose rule is that the set of silent drops is empty.
    // Dormant for the factory bank (deepest path: 8 levels) and live for anything deeper.
    Value deepest = object ({ { "leaf", number (1.0) } });

    for (int i = 0; i < 30; ++i)
        deepest = object ({ { "nested", std::move (deepest) } });

    const auto result = load (object ({ { "Flex0", std::move (deepest) } }));

    REQUIRE (result.ok);
    CHECK (wasReported (result, "nested deeper than 24 levels"));
}

TEST_CASE ("array length survives when the elements carry parameters, and not when they do not",
           "[structural]")
{
    // An array's length is implied by the highest index that stored something. A trailing element with
    // no parameters of its own leaves nothing behind, so the written array is shorter. Accepted: an
    // absent element and an empty one are the same state to a reader, and the reference's own reader
    // treats a missing index as default.
    const auto result = load (object ({ { "ArpClip0",
                                         object ({ { "clip",
                                                     object ({ { "notes",
                                                                 Value { [] {
                                                                             Value array;
                                                                             array.type = Value::Type::Array;
                                                                             array.items.push_back (object ({ { "channel", number (3.0) } }));
                                                                             array.items.push_back (object ({ { "strumType", text ("kEven") } }));
                                                                             return array;
                                                                         }() } } }) } }) } }));

    REQUIRE (result.ok);

    CHECK (result.document.parameters.count ("ArpClip0/clip/notes[0]/channel") == 1);
    CHECK (result.document.parameters.count ("ArpClip0/clip/notes[1]/strumType") == 1);

    const auto reread = roundTrip (result.document);

    // The elements that held a parameter come back, at the same indices — which is what makes an array's
    // *content* addressable even though its length is not stored.
    CHECK (reread.document.parameters.count ("ArpClip0/clip/notes[0]/channel") == 1);
    CHECK (reread.document.parameters.count ("ArpClip0/clip/notes[1]/strumType") == 1);
}

TEST_CASE ("spectral morphing after load is reported only where the engine cannot act on it", "[structural]")
{
    // Ticket 37 measured what the `clm` interpolation modes do, and it is not what the guide says: modes
    // 2/3/4 are a **plain linear crossfade**, which is what xerum already does. The real spectral morph —
    // magnitude interpolation with shortest-arc phase — is asked for by a *preset parameter*,
    // `interpolateAfterLoad == 2`, and it is in the signal path as of ticket 41: the plugin expands the
    // **embedded** table once at bind and the oscillator reads the morphed slots.
    //
    // So the report now tracks what the engine can act on rather than what it once could not. A request
    // the engine honours — an embedded table — is silent; one it cannot act on, a slot with no embedded
    // table to morph, is named at `high`.
    const auto preset = [] (const Value& afterLoad, const bool embedded)
    {
        Value slot = object ({ { "plainParams", object ({}) }, { "interpolateAfterLoad", afterLoad } });

        if (embedded)
            slot.entries.emplace_back ("embeddedWTData",
                                       arrayOf ({ number (0.0), number (0.5), number (-0.5), number (0.0) }));

        return load (object ({ { "Oscillator0", object ({ { "WTOsc0", std::move (slot) } }) } }));
    };

    // The request the engine honours, and all three of the field's values with it: an embedded table.
    // Nothing is reported, because nothing is dropped.
    for (const auto value : { 0LL, 1LL, 2LL })
    {
        const auto honoured = preset (integer (value), /*embedded*/ true);

        REQUIRE (honoured.ok);
        CHECK_FALSE (wasReported (honoured, "spectral morphing after load"));
    }

    // The one it cannot act on: `2` on a slot with no embedded table, so there is nothing to morph.
    const auto orphan = preset (integer (2), /*embedded*/ false);

    REQUIRE (orphan.ok);
    CHECK (wasReported (orphan, "spectral morphing after load"));
    CHECK (orphan.report.worst() == Severity::High);

    // And the neighbouring values are not complaints even with no table at all: 0 and 1 are the modes the
    // read already performs.
    for (const auto value : { 0LL, 1LL })
    {
        const auto plain = preset (integer (value), /*embedded*/ false);

        CHECK_FALSE (wasReported (plain, "spectral morphing after load"));
    }
}

// ---------------------------------------------------------------------------------------------------
// Ticket 39 — the modulation row: a field whose shape was checked instead of its meaning.
// ---------------------------------------------------------------------------------------------------

TEST_CASE ("every field the schema declares for a modulation row is applied or named", "[structural]")
{
    // Ticket 39's general rule, over the **whole** field space rather than the fields the factory bank
    // happens to use. The schema declares the fields a row can hold, `dsp::ModSlot` declares the ones the
    // engine reads, and the loader has to account for every difference between them. Written this way a
    // fourth silent field is not a matter of remembering to add a case: it fails here until it is either
    // applied by the engine or named by the loader.
    std::map<std::string, std::string> kindOf;   // row field -> the schema's kind for its leaf

    for (const auto& leaf : generated::kLeaves)
    {
        const std::string_view path = leaf.path;

        if (! path.starts_with ("ModSlot{N}/"))
            continue;

        const auto relative = path.substr (std::string_view ("ModSlot{N}/").size());

        if (relative == "plainParams")
            continue;   // the parameter container and its `"default"` sentinel: the case above covers it

        kindOf.emplace (rowFieldKey (relative), std::string (leaf.kind));
    }

    // Six fields of the row itself (`source`, the four `destModule*`, `flex`) and fifteen under
    // `plainParams`. A field that appears in the schema is answered for below, one way or the other.
    CHECK (kindOf.size() == 21);

    for (const auto field : kEngineRowFields)
    {
        INFO ("the engine acts on " << field);
        CHECK (kindOf.count (std::string (field)) == 1);
    }

    for (const auto& [field, kind] : kindOf)
    {
        const auto result = load (object ({ { "ModSlot0",
                                             modulationRowWith (field, valueForField (field, kind)) } }));
        REQUIRE (result.ok);

        const auto item = std::string ("ModSlot{N}/") + field;
        const auto* entry = entryForItem (result, item);

        INFO (field << " is a '" << kind << "' field, "
                    << (engineActsOn (field) ? "which the engine acts on" : "which the engine drops"));

        if (engineActsOn (field))
        {
            // Nothing about this row may be a complaint — the field is read, the destination resolves,
            // the row applies. That is what makes the other half of the rule meaningful: when a field is
            // *not* applied, the entry below is attributable to it and not to the row around it.
            CHECK (entry == nullptr);
            CHECK (result.report.clean());
        }
        else
        {
            // Named, at `medium`. The contract's `high` is for a sound-producing component or asset on
            // the audio path (an unsupported engine, warp, filter, effect, asset); a row's curve, trim or
            // smoothing shapes a **control signal** whose destination is still modulated, which is §3's
            // own definition of `medium` — "something was present that we did not interpret", whose
            // examples are an unknown module or parameter.
            REQUIRE (entry != nullptr);
            CHECK (entry->severity == Severity::Medium);
            CHECK (entry->modulePath == "ModSlot{N}");
            UNSCOPED_INFO ("  " << item << ": " << entry->reason);        }
    }
}

TEST_CASE ("a modulation row's dropped fields are named and kept", "[structural]")
{
    // The three fields ticket 27 measured as stored-and-dropped: the drawn transfer curve, the output
    // trim, and the aux invert. All three are ordinary schema leaves, so all three loaded in silence —
    // the same failure as the voice filter type nobody checked (tickets 23/26) and the subtree that
    // returned without a word (ticket 36): a field's shape standing in for its meaning.
    const auto result = load (object ({ { "ModSlot0",
                                         object ({ { "source", arrayOf ({ number (1.0), number (0.0) }) },
                                                   { "destModuleTypeString", text ("LFO") },
                                                   { "destModuleID", number (0.0) },
                                                   { "destModuleParamID", number (0.0) },
                                                   { "destModuleParamName", text ("kParamRate") },
                                                   { "flex", drawnCurve() },
                                                   { "plainParams", object ({ { "kParamAmount", number (50.0) },
                                                                              { "kParamOut", number (40.0) },
                                                                              { "kParamAuxInverted",
                                                                                number (1.0) } }) } }) } }));
    REQUIRE (result.ok);

    for (const auto* item : { "ModSlot{N}/flex",
                              "ModSlot{N}/plainParams/kParamOut",
                              "ModSlot{N}/plainParams/kParamAuxInverted" })
    {
        const auto* entry = entryForItem (result, item);
        REQUIRE (entry != nullptr);
        CHECK (entry->severity == Severity::Medium);
        UNSCOPED_INFO (item << ": " << entry->reason);
    }

    // Reported **and** retained. Reporting satisfies "nothing is silently dropped" for a load; dropping
    // the value from the document would make the *write* lose a field the reference put there, which is
    // the other half of the contract and the rule the unknown-parameter case above is built on.
    CHECK (result.document.parameters.count ("ModSlot0/flex[0]/xVals[1]") == 1);
    CHECK (result.document.parameters.count ("ModSlot0/plainParams/kParamOut") == 1);

    const auto reread = roundTrip (result.document);

    CHECK (reread.document.parameters.count ("ModSlot0/flex[0]/yVals[1]") == 1);
    CHECK (reread.document.parameters.count ("ModSlot0/plainParams/kParamOut") == 1);
    CHECK (reread.document.parameters.count ("ModSlot0/plainParams/kParamAuxInverted") == 1);

    // An **empty** curve is not a complaint: the reference writes `{}` for a curve it has nothing to say
    // about, which is the difference between "nothing drawn" and "a drawing we dropped" — 51 of the
    // corpus's 504 `flex` arrays hold two empty curves and cost nothing.
    const auto empty = load (object ({ { "ModSlot0",
                                         modulationRowWith ("flex",
                                                            arrayOf ({ object ({}), object ({}) })) } }));
    REQUIRE (empty.ok);
    CHECK (entryForItem (empty, "ModSlot{N}/flex") == nullptr);

    // A third source element is the same kind of loss the other way round: `source` is a field the engine
    // *does* read, but only its first two entries — the reference's pair is `[source, auxSource]`. The
    // corpus never stores one (all 13,258 rows hold exactly two), so this is the only place the check is
    // exercised, which is why it is asserted here rather than only counted there.
    const auto thirdSource = load (object ({ { "ModSlot0",
                                               modulationRowWith ("source",
                                                                  arrayOf ({ number (1.0), number (0.0),
                                                                             number (5.0) })) } }));
    REQUIRE (thirdSource.ok);

    const auto* extra = entryForItem (thirdSource, "ModSlot{N}/source[]");
    REQUIRE (extra != nullptr);
    CHECK (extra->severity == Severity::Medium);
    CHECK (thirdSource.document.parameters.count ("ModSlot0/source[2]") == 1);
}

TEST_CASE ("a modulation destination the parameter model cannot describe is named", "[structural]")
{
    // 128 rows in the factory bank modulate `Global.kParamMasterTuning` and 7 modulate a parameter of
    // `RetriggerState`. Both addresses resolve — the matrix computes a value for them — and the model
    // describes neither, so all 135 imported in silence: the loader asked whether `destModuleParamName`
    // was a string and never whether it named a parameter the destination's class actually has.
    const auto global = load (object ({ { "ModSlot0", rowTo ("Global", "kParamMasterTuning") } }));
    REQUIRE (global.ok);

    const auto* named = entryForItem (global, "Global{N}/plainParams/kParamMasterTuning");
    REQUIRE (named != nullptr);
    CHECK (named->severity == Severity::Medium);
    CHECK (named->modulePath == "ModSlot{N}");
    UNSCOPED_INFO ("destination: " << named->reason);

    // The complaint is about the *parameter*, not the class: the reference's Global module does have a
    // tuning parameter the model describes (`kParamGlobalTuning`, in the reference's init state), and a
    // row naming it is silent.
    const auto known = load (object ({ { "ModSlot0", rowTo ("Global", "kParamGlobalTuning") } }));
    REQUIRE (known.ok);
    CHECK (entryForItem (known, "Global{N}/plainParams/kParamGlobalTuning") == nullptr);

    // A class the model cannot describe at all: `RetriggerState` has a `plainParams` and no leaves.
    const auto retrigger = load (object ({ { "ModSlot0", rowTo ("RetriggerState", "kParamLFO1") } }));
    REQUIRE (retrigger.ok);
    CHECK (entryForItem (retrigger, "RetriggerState{N}/plainParams/kParamLFO1") != nullptr);

    // An FX destination resolves inside the rack's array, which is the one shape the class-to-module
    // mapping has to get right beyond `<Class>{N}/plainParams/`.
    const auto effect = load (object ({ { "ModSlot0", rowTo ("FXDistortion", "kParamDrive") } }));
    REQUIRE (effect.ok);
    CHECK (effect.report.clean());

    // The **name** is the identity, not the id — `destModuleParamID` is version-dependent (`Oscillator[10]`
    // is `kParamDetune` at 3.0 and `kParamStart` at 4.0), so the id cannot be asked this question at all:
    // a row naming a parameter the model has is silent whatever id it carries.
    const auto byName = load (object ({ { "ModSlot0",
                                         object ({ { "source", arrayOf ({ number (1.0), number (0.0) }) },
                                                   { "destModuleTypeString", text ("LFO") },
                                                   { "destModuleID", number (7.0) },
                                                   { "destModuleParamID", number (59.0) },
                                                   { "destModuleParamName", text ("kParamRate") },
                                                   { "plainParams", object ({}) } }) } }));
    REQUIRE (byName.ok);
    CHECK (byName.report.clean());

    // A row that names a source and no destination: the 6 rows in `SY - Tetris`. An *absent* field has no
    // shape to validate, which is why only a meaning-check can see this one at all.
    const auto noDestination = load (object ({ { "ModSlot0",
                                                 object ({ { "source", arrayOf ({ number (1.0), number (0.0) }) },
                                                           { "plainParams",
                                                             object ({ { "kParamAmount", number (50.0) } }) } }) } }));
    REQUIRE (noDestination.ok);

    const auto* missing = entryForItem (noDestination, "ModSlot{N}/destination");
    REQUIRE (missing != nullptr);
    CHECK (missing->severity == Severity::Medium);

    // ...and a row the reference left present but empty is not. Every preset carries all 64 rows, most of
    // them with nothing in them, so an empty row must stay silent or the report would have 64 bogus
    // entries per file.
    const auto unused = load (object ({ { "ModSlot7", object ({ { "plainParams", text ("default") } }) } }));
    REQUIRE (unused.ok);
    CHECK (unused.report.clean());
}
