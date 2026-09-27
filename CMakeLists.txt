cmake_minimum_required(VERSION 3.25)
project(AscensionExtensions LANGUAGES C CXX)

# Our own Extensions.dll for Ascension.exe (stock Wow.exe 12340 + a LoadLibraryA("Extensions.dll")
# cave), built on the MIT WotLK-Extensions DLL sources vendored under WotLKExtensions/.
# No Patcher, no injector: the exe loads a DLL of this name by itself.

set(CMAKE_CXX_STANDARD 17)
# static CRT everywhere (FreeType included): the DLL must not depend on a VC redistributable
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- upstream option set (same names as WotLK-Extensions' PatchConfig.hpp.in) --------------------
option(CUSTOM_DBC "Custom DBC loader" OFF)
option(LFGROLES_DBC "" OFF)
option(ZONELIGHT_DBC "" OFF)
option(OCCLUSIONVOLUME_DBC "" OFF)
option(SPELLATTRIBUTESEXTENDED_DBC "" OFF)
option(NOAMMO_PATCH "" OFF)
option(CHARCREATIONRACE_FIX "" ON)
option(COMBOPOINT_FIX "" ON)
option(CLASSLFDROLES_PATCH "" ON)
option(OOBLUAFUNCTIONS_PATCH "Register Lua functions from outside wow.exe" ON)
option(LUAUNLOCK_PATCH "" OFF)
option(CUSTOMPACKETS_PATCH "Custom opcode dispatch" ON)
option(ITEMMODEXT_PATCH "" OFF)
option(SPELLDESCRIPTIONEXT_PATCH "" OFF)
option(TOOLTIPID_PATCH "" OFF)
option(TOOLTIPDKRUNEEXT_PATCH "" OFF)
option(ITEMTOOLTIPMAPLINE_FIX "" OFF)
option(FRAMEEVENTEXTENSION "Register new FrameXML events" ON)
option(ACTIONBAR_LUA "" OFF)
option(DEVHELPER_LUA "" OFF)
option(GLUEMGREXTENSION "Glue-stage Lua function registration" ON)

set(UP ${CMAKE_CURRENT_SOURCE_DIR}/WotLKExtensions)
configure_file(${UP}/cmake/PatchConfig.hpp.in ${CMAKE_BINARY_DIR}/gen/PatchConfig.hpp)

file(GLOB_RECURSE UP_SOURCES CONFIGURE_DEPENDS ${UP}/src/*.cpp)
list(FILTER UP_SOURCES EXCLUDE REGEX ".*/src/Main\\.cpp$")   # we own DllMain
# Upstream feature patch sets the original client never applies (live exactness audit, 2026-09-27).
list(FILTER UP_SOURCES EXCLUDE REGEX ".*/src/Client/(CGTooltip|WoWTime|CMissile|CMap|Misc|MacroConditions|Spell)\\.cpp$")
list(FILTER UP_SOURCES EXCLUDE REGEX ".*/src/GameObjects/CGPlayer\\.cpp$")
list(FILTER UP_SOURCES EXCLUDE REGEX ".*/src/Misc/LuaUnlock\\.cpp$")
# Scaffold call-site hooks replaced by the original's own detours (audit step 3, 2026-09-27).
list(FILTER UP_SOURCES EXCLUDE REGEX ".*/src/Client/(CGlueMgr|CustomLua|CustomFileIO)\\.cpp$")
file(GLOB_RECURSE ASC_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_library(Extensions SHARED ${UP_SOURCES} ${ASC_SOURCES})
set_target_properties(Extensions PROPERTIES OUTPUT_NAME "Extensions" PREFIX "")
target_include_directories(Extensions PRIVATE ${CMAKE_BINARY_DIR}/gen ${UP}/src ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_compile_definitions(Extensions PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)

if(MSVC)
    # static CRT: the DLL must not depend on a VC redistributable on the player's box
    set_property(TARGET Extensions PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(Extensions PRIVATE /W3 /EHsc /Zi)
    target_link_options(Extensions PRIVATE /DEBUG /SAFESEH:NO)
endif()

# --- FreeType 2.14.1, as the original links it (FontRender.cpp; see AscFontRender.cpp) -------------
# Stock ftmodule.h / ftoption.h: every default module (incl. sdf, bsdf, ot-svg), internal zlib 1.3.1,
# LZW; no bzip2 / png / harfbuzz / brotli -- measured from the original's strings and FT_New_Library.
set(FT_DISABLE_BZIP2 TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_ZLIB FALSE CACHE BOOL "" FORCE)
set(FT_REQUIRE_ZLIB FALSE CACHE BOOL "" FORCE)
set(CMAKE_DISABLE_FIND_PACKAGE_ZLIB TRUE)   # always the bundled zlib, never a system one
set(SKIP_INSTALL_ALL TRUE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/third_party/freetype ${CMAKE_BINARY_DIR}/freetype EXCLUDE_FROM_ALL)
target_link_libraries(Extensions PRIVATE freetype)
