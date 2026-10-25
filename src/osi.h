// Invoking Osiris functions through the game's DIV dispatch handlers.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bg3le::osi {

// TOsiValueType. 1-5 are built in; 6+ are the story's own enum types.
enum ValueType : unsigned short {
    kNone = 0,
    kInteger = 1,
    kInteger64 = 2,
    kReal = 3,
    kString = 4,
    kGuidString = 5,
};

// The low 3 bits of a function id give its kind. 1-3 are the engine's own
// functions; the rest belong to the story -- a Proc_* a mod calls is kind
// 5, a user query kind 8.
enum Kind {
    kCall = 1,
    kQuery = 2,
    kEvent = 3,
    kDatabase = 4,
    kProc = 5,
    kSysQuery = 6,
    kSysCall = 7,
    kUserQuery = 8,
};

struct Function {
    std::string name;
    std::uint32_t id = 0;
    std::vector<std::uint8_t> params;

    // How many parameters the engine fills in. Known only once the
    // signature database has been read; -1 until then, in which case the
    // caller's argument count decides the split.
    int out_params = -1;

    // Which ones, bit i for parameter i: not always the trailing ones
    // (TemplateIsInPartyInventory's count is its third of four).
    std::uint64_t out_mask = 0;

    // Whether parameter i is an output when a caller passes `inputs`.
    bool is_output(std::size_t i, std::size_t inputs) const {
        if (out_params < 0) return i >= inputs;
        return i < 64 && ((out_mask >> i) & 1) != 0;
    }

    Kind kind() const { return static_cast<Kind>(id & 7); }

    // Which of the two dispatch handlers runs it.
    bool is_query() const {
        const Kind k = kind();
        return k == kQuery || k == kSysQuery || k == kUserQuery;
    }
};

// Reads out-parameter counts and parameter types from Osiris' own function
// database, keyed by name. Returns the number recovered, or 0 if it could
// not be read at all.
//
// `story` identifies the compiled story, and the answer is cached under
// it: the database is a property of the story, and walking it costs a
// hundred and thirty thousand reads on the story thread during level
// load. Pass nullptr to walk unconditionally.
// `cached` reports whether the answer came from the store rather than a
// walk, so the caller can say which.
std::size_t load_out_param_counts(std::vector<Function>* functions,
                                  char const* story, bool* cached);

// The functions the story itself defines -- procedures, user queries and
// databases -- which the engine's own mapping does not list. `known` is
// what that mapping gave, so the same function is not returned twice.
std::vector<Function> story_functions(std::vector<Function> const& known);

// Every function the database names, callable or not, sorted by name and
// arity. For describing them rather than for binding them.
std::vector<Function> all_functions();

// How many the database held that story_functions could not return, which
// on this build is all of them: they carry no dispatch handle.
std::size_t story_function_count();

// An Osiris type resolved to one of the five built-in ones, since the story
// declares its own as aliases of those.
std::uint16_t base_type(std::uint16_t declared);

// How many nodes Osiris' node list holds, or 0 if it was not found.
std::size_t node_count();

// Looks for Osiris' string pool by working back from a string it is known
// to hold. A diagnostic: it scans the process twice and logs what it
// finds. BG3LE_PROBE_STRINGS=1 in the story path drives it.
void probe_strings(char const* text);

// A value crossing the boundary in either direction.
struct Value {
    unsigned short type = kNone;
    std::int64_t integer = 0;
    double real = 0.0;
    std::string text;
};

// Records the dispatch handlers lifted from the callback table.
void set_handlers(void* call, void* query);
bool ready();

enum class Status {
    kHandled,      // the engine ran it and reported success
    kRejected,     // the engine ran it and reported false
    kUnavailable,  // could not be invoked at all
};

// Whether the story defines a function called `name`, and whether every
// declaration of it is a database. Resolved on demand from the Lua side:
// the Function objects behind these are heap pointers that cannot be
// cached between runs, and recovering them walks Osiris' database.
// `real` receives the name as the story spells it, which can differ in
// case from what was asked for.
// `is_query` is set when the name is a user query (QRY_*), called with query().
bool story_function(char const* name, bool* is_database, std::string* real,
                    bool* is_query = nullptr);

// Runs a user query with its IN arguments, as upstream's OsiUserQuery:
// kHandled or kRejected is its answer, and `outputs` receives one value per
// OUT parameter (kNone when it failed). Osiris' thread only.
Status query(char const* name, std::vector<Value> const& inputs,
             std::vector<Value>* outputs, std::string* why);

// Retracts facts from a story database. A `kNone` argument is a wildcard
// for that column, as a nil is upstream.
Status remove(char const* key, std::vector<Value> const& args,
              std::string* why);

// Called when the story runs a procedure, raises an event, or puts a fact
// into a database or takes one out -- all of which are the same tuple
// operation. `event` is "before", "after", "beforeDelete" or
// "afterDelete", as upstream names them.
//
// Runs on the thread Osiris runs on, inside the engine's own call.
using TriggerFn = void (*)(char const* name, std::size_t arity,
                           char const* event, std::vector<Value> const& values);

void set_trigger_sink(TriggerFn fn);

// Starts watching, by replacing the tuple slots in the two node classes
// that hold tuples. Idempotent, and not done until something asks: until
// then every node keeps the engine's own pointers.
bool watch_story_triggers();

// Watches an engine call for listeners, as upstream's CallPreHook and
// CallPostHook: fire_call sends its arguments to the trigger sink before
// and after the engine runs it. False if fn is not an engine call.
bool watch_call(Function const& fn);
bool call_watched(std::uint32_t id);
void fire_call(std::uint32_t id, void const* args, char const* event);

// The facts a story database holds, one row per fact, typed as declared.
bool facts(char const* key, std::vector<std::vector<Value>>* rows);

// Runs a story-defined function -- a procedure, an event, or a database
// insert -- by putting a tuple into the node that stands for it. This is
// the only route to them: they carry no dispatch handle, so `invoke` and
// the DIV boundary cannot reach them.
//
// Must be called on the thread Osiris runs on. `args` are in the
// function's declared order and are converted to its declared types;
// `why` is filled in when the answer is kUnavailable.
Status insert(char const* key, std::vector<Value> const& args,
              std::string* why);

// Invokes fn with inputs, appending any out-parameters to outputs.
// Distinguishing kRejected from kUnavailable matters: a query returning
// false is a normal answer, not a failure.
Status invoke(const Function& fn, const std::vector<Value>& inputs,
              std::vector<Value>* outputs);

// A story type's base type (1-5) from the story header's aliases; the type
// itself while it is unknown.
std::uint16_t resolve_alias(std::uint16_t type);

// One declaration of a name, as upstream's OsirisNameCache registers it: its
// arity, how many of those are inputs, and its FunctionType (1-8).
struct Signature {
    std::uint32_t arity = 0;
    std::uint32_t inputs = 0;
    std::uint32_t type = 0;
};

// Every declaration of name in Osiris' function database, by arity.
std::vector<Signature> signatures(char const* name);

// The base types (1-5) of the inputs of name's declaration that takes `inputs`
// of them, in declared order; 0 for one the story header did not resolve.
// False when no declaration takes that many.
bool declared_inputs(char const* name, std::size_t inputs, std::vector<std::uint16_t>* types);

}  // namespace bg3le::osi
