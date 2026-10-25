// Noesis functions bg3se calls, forwarded to the game's own copies.
//
// Upstream reimplements these against Windows data layouts because it links
// no Noesis library. The Linux executable has each as a local symbol, so each
// is defined here as `jmp *slot`, and bg3le_noesis_resolve() fills the slots
// by mangled name. A slot that stays empty traps with a log line instead.

#include <cstdlib>
#include <cstring>
#include <string>

#include "log.h"

// Kept in step with what the vendored ClientUI code references:
// `nm -D --undefined-only libbg3le.so | grep Noesis` must print nothing.
#define BG3LE_NOESIS_FUNCTIONS(X) \
    X(_ZN6Noesis10BaseObjectdlEPv) \
    X(_ZN6Noesis9UIElement10RaiseEventERKNS_15RoutedEventArgsE) \
    X(_ZN6Noesis10BaseObjectnwEm) \
    X(_ZN6Noesis10BoxedValue18StaticGetClassTypeEPNS_7TypeTagIS0_EE) \
    X(_ZN6Noesis10Reflection12RegisterTypeENS_6SymbolEPFPNS_4TypeES1_EPFvS3_E) \
    X(_ZN6Noesis10Reflection12RegisterTypeEPKcPFPNS_4TypeENS_6SymbolEEPFvS4_E) \
    X(_ZN6Noesis10Reflection7GetTypeENS_6SymbolE) \
    X(_ZN6Noesis11BaseCommand17CanExecuteChangedEv) \
    X(_ZN6Noesis11BaseCommand18StaticGetClassTypeEPNS_7TypeTagIS0_EE) \
    X(_ZN6Noesis11BaseCommandC2Ev) \
    X(_ZN6Noesis11BaseCommandD2Ev) \
    X(_ZN6Noesis12TypePropertyC2ENS_6SymbolEPKNS_4TypeE) \
    X(_ZN6Noesis12TypePropertyD2Ev) \
    X(_ZN6Noesis13BaseComponent18StaticGetClassTypeEPNS_7TypeTagIS0_EE) \
    X(_ZN6Noesis13BaseComponentC2Ev) \
    X(_ZN6Noesis13BaseComponentD2Ev) \
    X(_ZN6Noesis13SymbolManager10FindStringEPKc) \
    X(_ZN6Noesis13SymbolManager15AddStaticStringEPKc) \
    X(_ZN6Noesis13SymbolManager9AddStringEPKc) \
    X(_ZN6Noesis13SymbolManager9GetStringEj) \
    X(_ZN6Noesis14BaseCollection12AddComponentEPNS_13BaseComponentE) \
    X(_ZN6Noesis14BaseCollection12SetComponentEjPNS_13BaseComponentE) \
    X(_ZN6Noesis14BaseCollection8RemoveAtEj) \
    X(_ZN6Noesis16TypeClassBuilder11AddPropertyEPNS_12TypePropertyE) \
    X(_ZN6Noesis16TypeClassBuilder12AddInterfaceEPKNS_9TypeClassEj) \
    X(_ZN6Noesis16TypeClassBuilder7AddBaseEPKNS_9TypeClassE) \
    X(_ZN6Noesis24BaseObservableCollection10ClearItemsEv) \
    X(_ZN6Noesis24BaseObservableCollection10InsertItemEjPNS_13BaseComponentE) \
    X(_ZN6Noesis24BaseObservableCollection10RemoveItemEj) \
    X(_ZN6Noesis24BaseObservableCollection15PropertyChangedEv) \
    X(_ZN6Noesis24BaseObservableCollection17CollectionChangedEv) \
    X(_ZN6Noesis24BaseObservableCollection18StaticGetClassTypeEPNS_7TypeTagIS0_EE) \
    X(_ZN6Noesis24BaseObservableCollection7SetItemEjPNS_13BaseComponentE) \
    X(_ZN6Noesis24BaseObservableCollection8MoveItemEii) \
    X(_ZN6Noesis24BaseObservableCollectionC2Ev) \
    X(_ZN6Noesis24BaseObservableCollectionD2Ev) \
    X(_ZN6Noesis4CastEPKNS_9TypeClassEPNS_10BaseObjectE) \
    X(_ZN6Noesis4Impl8ToStringEa) \
    X(_ZN6Noesis4Impl8ToStringEd) \
    X(_ZN6Noesis4Impl8ToStringEf) \
    X(_ZN6Noesis4Impl8ToStringEh) \
    X(_ZN6Noesis4Impl8ToStringEi) \
    X(_ZN6Noesis4Impl8ToStringEj) \
    X(_ZN6Noesis4Impl8ToStringEl) \
    X(_ZN6Noesis4Impl8ToStringEm) \
    X(_ZN6Noesis4Impl8ToStringERKNS_11FixedStringILj24EEE) \
    X(_ZN6Noesis4Impl8ToStringEs) \
    X(_ZN6Noesis4Impl8ToStringEt) \
    X(_ZN6Noesis5AllocEm) \
    X(_ZN6Noesis6Boxing10FalseBoxedEv) \
    X(_ZN6Noesis6Boxing14BoxingAllocateEm) \
    X(_ZN6Noesis6Boxing16BoxingDeallocateEPvm) \
    X(_ZN6Noesis6Boxing9TrueBoxedEv) \
    X(_ZN6Noesis6Visual14AddVisualChildEPS0_) \
    X(_ZN6Noesis7DeallocEPv) \
    X(_ZN6Noesis7ReallocEPvm) \
    X(_ZN6Noesis7TypePtr20SetStaticContentTypeEPKNS_4TypeE) \
    X(_ZN6Noesis7TypePtrC1ENS_6SymbolE) \
    X(_ZN6Noesis8TypeMetaC1ENS_6SymbolE) \
    X(_ZN6Noesis9TypeClassC1ENS_6SymbolEb) \
    X(_ZN6Noesis9UIElement10AddHandlerEPKNS_11RoutedEventERKNS_8DelegateIFvPNS_13BaseComponentERKNS_15RoutedEventArgsEEEE) \
    X(_ZN6Noesis9UIElement13RemoveHandlerEPKNS_11RoutedEventERKNS_8DelegateIFvPNS_13BaseComponentERKNS_15RoutedEventArgsEEEE) \
    X(_ZN6Noesis9UIElement18StaticGetClassTypeEPNS_7TypeTagIS0_EE) \
    X(_ZNK6Noesis10BaseObject6EqualsEPKS0_) \
    X(_ZNK6Noesis10BaseObject8ToStringEv) \
    X(_ZNK6Noesis13BaseComponent12GetClassTypeEv) \
    X(_ZNK6Noesis14BaseCollection12GetComponentEj) \
    X(_ZNK6Noesis14BaseCollection16IndexOfComponentEPKNS_13BaseComponentE) \
    X(_ZNK6Noesis14BaseCollection5CountEv) \
    X(_ZNK6Noesis4Rect8ToStringEv) \
    X(_ZNK6Noesis5Color8ToStringEv) \
    X(_ZNK6Noesis5Point8ToStringEv) \
    X(_ZNK6Noesis7Vector28ToStringEv) \
    X(_ZThn16_N6Noesis11BaseCommand17CanExecuteChangedEv) \
    X(_ZThn16_N6Noesis14BaseCollection12AddComponentEPNS_13BaseComponentE) \
    X(_ZThn16_N6Noesis14BaseCollection12SetComponentEjPNS_13BaseComponentE) \
    X(_ZThn16_NK6Noesis14BaseCollection12GetComponentEj) \
    X(_ZThn16_NK6Noesis14BaseCollection16IndexOfComponentEPKNS_13BaseComponentE) \
    X(_ZThn16_NK6Noesis14BaseCollection5CountEv) \
    X(_ZThn40_N6Noesis24BaseObservableCollection17CollectionChangedEv) \
    X(_ZThn48_N6Noesis24BaseObservableCollection15PropertyChangedEv)

namespace {

extern "C" [[noreturn]] void bg3le_noesis_unresolved() {
    bg3le::logf("noesis: called a Noesis function the game does not have; "
                "see the 'noesis: missing' lines above");
    std::abort();
}

}  // namespace

#define BG3LE_SLOT(m) \
    __attribute__((visibility("hidden"))) void* bg3le_nsfwd_##m = \
        (void*)&bg3le_noesis_unresolved;
extern "C" {
BG3LE_NOESIS_FUNCTIONS(BG3LE_SLOT)
}
#undef BG3LE_SLOT

#define BG3LE_STUB(m)                                        \
    ".globl " #m "\n.hidden " #m "\n.type " #m ",@function\n" \
    #m ":\n\tjmp *bg3le_nsfwd_" #m "(%rip)\n.size " #m ",.-" #m "\n"
__asm__(".text\n" BG3LE_NOESIS_FUNCTIONS(BG3LE_STUB));
#undef BG3LE_STUB

namespace {

struct Forward {
    char const* name;
    void** slot;
};

#define BG3LE_ENTRY(m) {#m, &bg3le_nsfwd_##m},
const Forward kForwards[] = {
    BG3LE_NOESIS_FUNCTIONS(BG3LE_ENTRY){nullptr, nullptr}};
#undef BG3LE_ENTRY

// C1/C2 and D1/D2 are one function when a class has no virtual bases, and
// the compiler may keep only one of the names.
std::string sibling(char const* name) {
    std::string s(name);
    for (char const* pair : {"C1E", "C2E", "D1E", "D2E"}) {
        const auto at = s.rfind(pair);
        if (at == std::string::npos) continue;
        s[at + 1] = pair[1] == '1' ? '2' : '1';
        return s;
    }
    return {};
}

}  // namespace

// Returns how many stayed unresolved.
extern "C" std::size_t bg3le_noesis_resolve(void* (*find)(char const*)) {
    std::size_t missing = 0;
    for (Forward const* f = kForwards; f->name != nullptr; ++f) {
        void* address = find(f->name);
        if (address == nullptr) {
            const std::string other = sibling(f->name);
            if (!other.empty()) address = find(other.c_str());
        }
        if (address == nullptr) {
            bg3le::logf("noesis: missing %s", f->name);
            ++missing;
            continue;
        }
        *f->slot = address;
    }
    return missing;
}
