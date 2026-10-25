#!/usr/bin/env python3
"""Verifies the clang fixes listed in vendor/NOTICE.md are still applied.

Re-copying files from an upstream bg3se checkout silently reverts them, which
has already happened twice. Run this after any such copy.
"""
import pathlib
import re
import sys

V = pathlib.Path(__file__).resolve().parent.parent / "vendor" / "bg3se"
SOURCES = [p for p in V.rglob("*") if p.is_file() and p.suffix in (".h", ".inl", ".cpp")]


def text(rel):
    return (V / rel).read_text(errors="replace")


def grep_count(pattern, flags=0):
    rx = re.compile(pattern, flags)
    return sum(1 for p in SOURCES if rx.search(p.read_text(errors="replace")))


CHECKS = [
    ("EntityHandleGenerator keeps ThreadStates at +0",
     lambda: "ThreadStates at +0x40 rather than +0" in text("BG3Extender/GameDefinitions/EntitySystem.h")),
    ("RPGStats has this build's fifth pointer before RNG",
     lambda: "field_280_bg3le" in text("BG3Extender/GameDefinitions/Stats/Stats.h")),
    ("UIStateMachine has this build's extra map and set",
     lambda: "field_170_bg3le" in text("BG3Extender/GameDefinitions/UI.h")
             and "field_240_bg3le" in text("BG3Extender/GameDefinitions/UI.h")
             and "field_F0_bg3le" in text("BG3Extender/GameDefinitions/UI.h")),
    ("custom property WriteCallback goes through bg3le's UI queue",
     lambda: "bg3le_ui_property_written(ptr, this->GetName().Str())" in text("BG3Extender/Lua/Libs/ClientUI/CustomProperties.inl")),
    ("a wrapped DataContext property registers its base with bg3le",
     lambda: "bg3le_ui_wrapped_property(this, baseProperty);" in text("BG3Extender/Lua/Libs/ClientUI/CustomProperties.inl")),
    ("ModifierList::Name in the manager's tail padding",
     lambda: "[[no_unique_address]] CNamedElementManager<Modifier> Attributes;" in text("BG3Extender/GameDefinitions/Stats/Stats.h")),
    ("entity handle generator grows by one page",
     lambda: "every new entry on the free list" in text("BG3Extender/GameDefinitions/EntitySystem.cpp")),
    ("requires-clause wrapped in parens",
     lambda: grep_count(r"requires !") == 0),
    ("no stray typename before a builtin type",
     lambda: grep_count(r"^typename (bool|void|int|float|double|char) ", re.M) == 0),
    ("FunctionImpl friend declaration names its parameters",
     lambda: "template <class TFun, class TData> friend class FunctionImpl;"
             in text("CoreLib/Base/BaseFunction.h")),
    ("__FUNCTION__ concatenation removed",
     lambda: '__FUNCTION__ "(): "' not in text("BG3Extender/Extender/Shared/Utils.h")),
    ("std::thread included rather than forward-declared",
     lambda: "#include <thread>" in text("BG3Extender/Extender/Shared/Utils.h")),
    ("SDL_HOOK specialisations marked template<> inline",
     lambda: "template<> inline SDL##name##HookType"
             in text("BG3Extender/Extender/Client/SDLManager.h")),
    ("LuaStats.h include uses the real directory case",
     lambda: "<Lua/LuaBinding.h>" in text("BG3Extender/Lua/Shared/LuaStats.h")),
    ("BuildInfo.h transcoded to UTF-8",
     lambda: (V / "BG3Extender/Extender/BuildInfo.h").read_bytes()[:2] != b"\xff\xfe"),
    ("Config.h transcoded to UTF-8",
     lambda: (V / "CoreLib/Config.h").read_bytes()[:2] != b"\xff\xfe"),
    ("log macros use __VA_OPT__",
     lambda: "__VA_OPT__" in text("CoreLib/Utils.h")),
    ("FixedStringUnhashed has a stream operator",
     lambda: "bg3se::FixedStringUnhashed const& str" in text("CoreLib/Base/BaseString.h")),
    ("FOR_NOESIS_TYPE specialisations marked template<> inline",
     lambda: "template<> inline Symbol SymbolInfo<T>::Name"
             in text("BG3Extender/Lua/Libs/ClientUI/Symbols.inl")),
    ("BaseObject::operator new takes size_t",
     lambda: "unsigned __int64" not in text("BG3Extender/Lua/Libs/ClientUI/Builtins.inl")),
    ("function pointer cast uses reinterpret_cast",
     lambda: "reinterpret_cast<Visual__PointConversionProc*>"
             in text("BG3Extender/Lua/Libs/ClientUI/NsHelpers.inl")),
    ("TryOpOrFail SFINAE uses a member template parameter",
     lambda: "class TT = T" in text("BG3Extender/Lua/Libs/Math.inl")),
    ("Json variant alternatives cast explicitly",
     lambda: "(int64_t)lua_tointeger" in text("BG3Extender/Lua/Libs/Json.inl")),
    ("ClientAudio passes c_str() to the variadic",
     lambda: "name.c_str()" in text("BG3Extender/Lua/Libs/ClientAudio.inl")),
    ("derived_from guarded by a completeness check",
     lambda: "IsCompleteType<T>" in text("BG3Extender/GameDefinitions/Base/TypeMetadata.h")),
    ("std::array extents deduced as size_t",
     lambda: "template <class T, std::size_t Size>"
             in text("BG3Extender/Lua/Shared/Proxies/LuaArrayProxy.h")),
    ("P_FALLBACK casts function pointers to void*",
     lambda: ".Getter = (void*)getter"
             in text("BG3Extender/Lua/Shared/Proxies/LuaObjectProxies.cpp")),
    ("fstream paths converted to UTF-8",
     lambda: "ToUTF8(resPath)" in text("BG3Extender/Lua/Shared/LuaBundle.cpp")),
    ("pointer-to-member conversion ported to the Itanium ABI",
     lambda: "sizeof(MethodType) >= sizeof(FunctionType)"
             in text("CoreLib/Wrappers.h")),
    ("PERF_REPORT has no leading ##",
     lambda: "WARN(##" not in text("BG3Extender/Extender/Shared/ExtenderConfig.h")),
    ("Hooks.cpp names its hook type",
     lambda: "decltype(Hooks::eocnet__ClientConnectMessage__Serialize)::gHook"
             not in text("BG3Extender/Extender/Shared/Hooks.cpp")),
    ("DebugInterface uses POSIX socket spellings",
     lambda: "S_un.S_addr" not in text("BG3Extender/Osiris/Debugger/DebugInterface.cpp")),
    ("guarded regions use try/catch rather than SEH",
     lambda: "HandleGuardedCppException"
             in text("BG3Extender/GameDefinitions/Base/Base.h")),
    ("push has long long overloads",
     lambda: "unsigned long long v" in text("BG3Extender/Lua/Helpers/LuaPush.h")),
    ("property map Definitions is inline const",
     lambda: "static inline const PropertyMapRegistrationEntry Definitions"
             in text("BG3Extender/Lua/Shared/Proxies/LuaObjectProxies.cpp")),
    ("static hook members defined through an alias",
     lambda: "decltype(ScriptExtender::CoreLibInit)::gHook"
             not in text("BG3Extender/Extender/ScriptExtender.cpp")),
    ("static member specialisations have initialisers",
     lambda: all("::gHook;" not in text(f) for f in (
         "BG3Extender/Extender/Client/SDLManager.h",
         "BG3Extender/Extender/Client/IMGUI/Vulkan.inl",
         "BG3Extender/Extender/ScriptExtender.cpp",
         "BG3Extender/Extender/Server/ScriptExtenderServer.cpp",
         "BG3Extender/Extender/Client/ScriptExtenderClient.cpp",
         "BG3Extender/Extender/Shared/Hooks.cpp"))),
    ("ImGui D3D11 backend gated",
     lambda: "BG3LE_NO_DX11"
             in text("BG3Extender/Extender/Client/IMGUI/IMGUI.cpp")),
    ("variable template specialisations are inline",
     lambda: all("template <> constexpr" not in text(f) and
                 "template<> constexpr" not in text(f) for f in (
         "BG3Extender/GameDefinitions/Base/TypeMetadata.h",
         "BG3Extender/GameDefinitions/Base/LuaAnnotations.h",
         "BG3Extender/GameDefinitions/Enumerations.h",
         "BG3Extender/Lua/Shared/Proxies/LuaStructIDs.h"))),
    ("VMCallEntry constructor is not declared inline",
     lambda: "inline VMCallEntry(State* state" not in text("BG3Extender/Lua/LuaBinding.h")),
    ("NsCustomDataContext has a usual operator delete",
     lambda: "static void operator delete(void* ptr) noexcept"
             in text("BG3Extender/Lua/Libs/ClientUI/CustomProperties.inl")),

    # These three are not clang fixes; they are the places bg3le had to
    # change upstream's behaviour rather than its syntax. Losing one is
    # silent and expensive, so they are checked the same way.
    ("MakeFileReader reads the game's archives",
     lambda: "bg3le::make_data_file_reader"
             in text("BG3Extender/GameDefinitions/GameHelpers.cpp")),
    ("SDLManager has the Linux forwarder entry points",
     lambda: "int OnPollEvent(SDLPollEventProc* wrapped, SDL_Event* event);"
             in text("BG3Extender/Extender/Client/SDLManager.h")),
    ("LuaDelegate posts to bg3le's callback queue",
     lambda: "bg3le::delegate_post"
             in text("BG3Extender/Lua/Shared/LuaDelegate.h")),
    ("handled-error hook tolerates a missing gExtender",
     lambda: "if (!bg3se::gExtender) return;"
             in text("BG3Extender/Lua/LuaBinding.cpp")),
    ("BindIcon tolerates a missing texture atlas map",
     lambda: "ls__gTextureAtlasMap == nullptr" in text("BG3Extender/Extender/Client/IMGUI/IMGUI.cpp")),
    ("IMGUIManager::Update draws ShowErrorAndExitGame's dialog",
     lambda: "bg3le_imgui_draw_error();" in text("BG3Extender/Extender/Client/IMGUI/IMGUI.cpp")),
    ("the IMGUI texture loader takes engine references through bg3le",
     lambda: "bg3le_texture_acquire(textureGuid.Index)" in text("BG3Extender/Extender/Client/IMGUI/IMGUI.cpp")
             and "bg3le_texture_release(it.Key().Index)" in text("BG3Extender/Extender/Client/IMGUI/IMGUI.cpp")),
    ("Noesis builtins forward to the game",
     lambda: "#include <bg3le_noesis_builtins.inl>" in text("BG3Extender/Lua/Libs/ClientUI/Builtins.inl")
             and text("BG3Extender/GameDefinitions/UI.h").count("#if !defined(BG3LE_NOESIS_FORWARD)") == 2),
    ("Ext.UI.GetRoot uses the View and the bridge is included",
     lambda: "bg3le::noesis_root()" in text("BG3Extender/Lua/Libs/ClientUI/Module.inl")
             and "#include <bg3le_noesis_lua.inl>" in text("BG3Extender/Lua/Libs/ClientUI/Module.inl")),
    ("the Vulkan overlay is composited onto an HDR swapchain",
     lambda: "bg3le::hdr_record(image.commandBuffer" in text("BG3Extender/Extender/Client/IMGUI/Vulkan.inl")
             and "bg3le::hdr_swapchain_created(" in text("BG3Extender/Extender/Client/IMGUI/Vulkan.inl")),
    ("the Vulkan overlay draws only live image views, and images re-bind dead ones",
     lambda: text("BG3Extender/Extender/Client/IMGUI/Vulkan.inl").count("bg3le::vk_image_view_live(") == 3
             and "!gExtender->IMGUI().IsTextureLive(TextureHandle)" in text("BG3Extender/Extender/Client/IMGUI/IMGUI.cpp")),
    ("the Vulkan overlay keeps one pool and sampler per device and locks binds",
     lambda: "if (descriptorPool_ == VK_NULL_HANDLE) {" in text("BG3Extender/Extender/Client/IMGUI/Vulkan.inl")
             and "if (sampler_ == VK_NULL_HANDLE) {" in text("BG3Extender/Extender/Client/IMGUI/Vulkan.inl")
             and "ImGui_ImplVulkan_AddTexture cannot run on a shut-down backend" in text("BG3Extender/Extender/Client/IMGUI/Vulkan.inl")),
    ("icon atlases register their resident texture",
     lambda: "reinterpret_cast<TextureDescriptor*>(atlas->Texture)" in text("BG3Extender/Extender/Client/IMGUI/IMGUI.cpp")
             and "bool Resident{ false };" in text("BG3Extender/Extender/Client/IMGUI/IMGUI.h")),
    ("GlobalSwitches has this build's SoundSetting size and tail padding",
     lambda: "uint64_t field_58_bg3le;" in text("BG3Extender/GameDefinitions/Misc.h")
             and "uint64_t field_12F0_bg3le;" in text("BG3Extender/GameDefinitions/Misc.h")),
]


def main():
    failed = 0
    for name, check in CHECKS:
        try:
            ok = check()
        except Exception as exc:  # a missing file is a failure, not a crash
            print(f"  ERR  {name}: {exc}")
            failed += 1
            continue
        print(f"  {'OK  ' if ok else 'FAIL'} {name}")
        failed += not ok
    if failed:
        print(f"\n{failed} vendored fix(es) missing; see vendor/NOTICE.md")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
